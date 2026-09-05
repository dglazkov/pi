import assert from "node:assert/strict";
import { createRequire, Module } from "node:module";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { getNativeClipboard, getNativePlatformHelper } from "../src/native-platform.ts";

// Opt in on a Windows test desktop: this replaces the system clipboard contents.
test(
	"writes Windows clipboard text through the native helper without command fallbacks",
	{ skip: process.platform !== "win32" || process.env.PI_TEST_NATIVE_CLIPBOARD !== "1" },
	() => {
		const clipboard = getNativeClipboard();
		assert.ok(clipboard?.setText);
		for (const text of ["clipboard café 日本語", "", "second write"]) {
			clipboard.setText(text);
			assert.equal(clipboard.getText(), text);
			assert.equal(clipboard.getImage(), null);
		}
	},
);

test(
	"uses the native platform helper directly as the clipboard API",
	{ skip: !["darwin", "win32"].includes(process.platform) || !["arm64", "x64"].includes(process.arch) },
	() => {
		const clipboard = getNativeClipboard();
		assert.ok(clipboard);
		assert.equal(typeof clipboard.getText, "function");
		assert.equal(typeof clipboard.getImage, "function");
		assert.equal(typeof clipboard.setText, "function");
		assert.equal(clipboard, getNativePlatformHelper());
		assert.equal(clipboard, getNativeClipboard());
	},
);

test(
	"Linux reads once, retries unavailable displays, and respects explicit backend selection",
	{ skip: !["arm64", "x64"].includes(process.arch) },
	(t) => {
		const require = createRequire(import.meta.url);
		const platform = Object.getOwnPropertyDescriptor(process, "platform")!;
		const display = process.env.DISPLAY;
		const waylandDisplay = process.env.WAYLAND_DISPLAY;
		t.after(() => {
			Object.defineProperty(process, "platform", platform);
			if (display === undefined) delete process.env.DISPLAY;
			else process.env.DISPLAY = display;
			if (waylandDisplay === undefined) delete process.env.WAYLAND_DISPLAY;
			else process.env.WAYLAND_DISPLAY = waylandDisplay;
		});
		Object.defineProperty(process, "platform", { value: "linux" });
		process.env.DISPLAY = ":0";
		process.env.WAYLAND_DISPLAY = "wayland-0";

		let waylandAvailable = false;
		const wayland = {
			getText: t.mock.fn(() => (waylandAvailable ? null : undefined)),
			getImage: t.mock.fn(() => (waylandAvailable ? null : undefined)),
		};
		const x11 = { getText: t.mock.fn(() => "X11"), getImage: t.mock.fn(() => new Uint8Array([1])) };
		for (const [backend, helper] of [
			["wayland", wayland],
			["x11", x11],
		] as const) {
			const modulePath = fileURLToPath(
				new URL(`../native/linux/prebuilds/linux-${process.arch}/linux-platform-${backend}.node`, import.meta.url),
			);
			const previous = require.cache[modulePath];
			const module = new Module(modulePath);
			module.exports = helper;
			require.cache[modulePath] = module;
			t.after(() => {
				if (previous) require.cache[modulePath] = previous;
				else delete require.cache[modulePath];
			});
		}

		const clipboard = getNativeClipboard()!;
		assert.equal(getNativeClipboard("wayland"), wayland);
		assert.equal(getNativeClipboard("x11"), x11);
		for (const method of ["getText", "getImage"] as const) {
			assert.equal(wayland[method].mock.callCount(), 0); // Loading does not probe the display.
			assert.equal(x11[method].mock.callCount(), 0);
			waylandAvailable = false;
			assert.deepEqual(clipboard[method](), method === "getText" ? "X11" : new Uint8Array([1]));
			assert.equal(wayland[method].mock.callCount(), 1);
			assert.equal(x11[method].mock.callCount(), 1);
			waylandAvailable = true;
			assert.equal(clipboard[method](), null); // Empty stops fallback; availability was not cached.
			assert.equal(wayland[method].mock.callCount(), 2);
			const failedRead = t.mock.method(wayland, method, () => {
				throw new Error("transfer failed");
			});
			assert.throws(() => clipboard[method](), /transfer failed/);
			failedRead.mock.restore();
			assert.equal(x11[method].mock.callCount(), 1);
			assert.equal(clipboard[method](), null);
		}

		delete process.env.DISPLAY;
		delete process.env.WAYLAND_DISPLAY;
		assert.equal(getNativeClipboard(), undefined);
		process.env.WAYLAND_DISPLAY = "wayland-0";
		assert.equal(getNativeClipboard(), clipboard);
		assert.equal(getNativeClipboard("x11"), undefined);
	},
);
