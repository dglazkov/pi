import type { NativeClipboard } from "@earendil-works/pi-tui";
import type { SpawnSyncReturns } from "child_process";
import { writeFileSync } from "fs";
import { beforeEach, describe, expect, test, vi } from "vitest";
import { readClipboardImage } from "../src/utils/clipboard-image.ts";

const mocks = vi.hoisted(() => ({
	spawnSync: vi.fn<(command: string, args: string[], options: unknown) => SpawnSyncReturns<Buffer>>(),
	getImage: vi.fn<() => Uint8Array | null | undefined>(),
	getNativeClipboard: vi.fn<(backend?: "wayland" | "x11") => NativeClipboard | undefined>(),
}));

vi.mock("child_process", () => ({ spawnSync: mocks.spawnSync }));
vi.mock("@earendil-works/pi-tui", () => ({ getNativeClipboard: mocks.getNativeClipboard }));

function spawnResult(stdout: Buffer, status = 0): SpawnSyncReturns<Buffer> {
	return {
		pid: 123,
		output: [Buffer.alloc(0), stdout, Buffer.alloc(0)],
		stdout,
		stderr: Buffer.alloc(0),
		status,
		signal: null,
	};
}

const png = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52]);

describe("readClipboardImage", () => {
	beforeEach(() => {
		vi.resetAllMocks();
		mocks.spawnSync.mockReturnValue(spawnResult(Buffer.alloc(0), 1));
		mocks.getImage.mockReturnValue(png);
		mocks.getNativeClipboard.mockReturnValue({ getText: () => null, getImage: mocks.getImage });
	});

	for (const [backend, command, env] of [
		["wayland", "wl-paste", { WAYLAND_DISPLAY: "1", DISPLAY: ":0" }],
		["x11", "xclip", { DISPLAY: ":0" }],
	] as const) {
		test.each([true, false])(`${backend}: command image present=%s stops fallback`, async (present) => {
			mocks.spawnSync.mockImplementation((name, args) => {
				expect(name).toBe(command);
				const listing = args.includes("--list-types") || args.includes("TARGETS");
				return spawnResult(listing ? Buffer.from(present ? "text/plain\nimage/png\n" : "text/plain\n") : png);
			});
			expect(await readClipboardImage({ platform: "linux", env })).toEqual(
				present ? { bytes: png, mimeType: "image/png" } : null,
			);
			expect(mocks.spawnSync).toHaveBeenCalledTimes(present ? 2 : 1);
			expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
		});

		test.each([png, null, new Uint8Array()])(`${backend}: native result %j stops fallback`, async (bytes) => {
			mocks.getImage.mockReturnValue(bytes);
			expect(await readClipboardImage({ platform: "linux", env })).toEqual(
				bytes?.length ? { bytes, mimeType: "image/png" } : null,
			);
			expect(mocks.getNativeClipboard).toHaveBeenCalledExactlyOnceWith(backend);
			expect(mocks.getImage).toHaveBeenCalledOnce();
			expect(mocks.spawnSync.mock.calls.map(([name]) => name)).toEqual(
				Array<string>(backend === "wayland" ? 1 : 5).fill(command),
			);
		});
	}

	test.each(["missing module", "unavailable display"])("Wayland: falls back to X11 after %s", async (failure) => {
		if (failure === "missing module") mocks.getNativeClipboard.mockReturnValue(undefined);
		else mocks.getImage.mockReturnValue(undefined);
		mocks.spawnSync.mockImplementation((command, args) => {
			if (command === "wl-paste") return spawnResult(Buffer.alloc(0), 1);
			return spawnResult(args.includes("TARGETS") ? Buffer.from("image/png\n") : Buffer.from(png));
		});
		expect(await readClipboardImage({ platform: "linux", env: { WAYLAND_DISPLAY: "1" } })).toEqual({
			bytes: png,
			mimeType: "image/png",
		});
		expect(mocks.getNativeClipboard).toHaveBeenCalledExactlyOnceWith("wayland");
	});

	test("WSL: passes the PowerShell path directly instead of through a custom env var", async () => {
		mocks.getImage.mockReturnValue(null);
		let tmpFile: string | undefined;
		mocks.spawnSync.mockImplementation((command, args, options) => {
			if (command === "wl-paste" || command === "xclip") return spawnResult(Buffer.alloc(0));
			if (command === "wslpath") {
				tmpFile = args[1];
				return spawnResult(Buffer.from("C:\\Users\\O'Hare\\clip.png\n"));
			}
			if (command === "powershell.exe") {
				const spawnOptions = options as { env?: NodeJS.ProcessEnv };
				expect(spawnOptions.env?.PI_WSL_CLIPBOARD_IMAGE_PATH).toBeUndefined();
				expect(args[2]).toContain("$path = 'C:\\Users\\O''Hare\\clip.png'");
				if (!tmpFile) throw new Error("wslpath should be called before powershell.exe");
				writeFileSync(tmpFile, png);
				return spawnResult(Buffer.from("ok\n"));
			}
			throw new Error(`Unexpected command: ${command}`);
		});
		expect(await readClipboardImage({ platform: "linux", env: { WSL_DISTRO_NAME: "Ubuntu" } })).toEqual({
			bytes: new Uint8Array(png),
			mimeType: "image/png",
		});
	});

	for (const platform of ["darwin", "win32"] as const) {
		test.each([png, null, new Uint8Array(), undefined])(`${platform}: reads native image %j once`, async (bytes) => {
			mocks.getImage.mockReturnValue(bytes);
			expect(await readClipboardImage({ platform, env: {} })).toEqual(
				bytes?.length ? { bytes, mimeType: "image/png" } : null,
			);
			expect(mocks.getImage).toHaveBeenCalledOnce();
			expect(mocks.spawnSync).not.toHaveBeenCalled();
		});
	}

	test("returns null without a native helper", async () => {
		mocks.getNativeClipboard.mockReturnValue(undefined);
		expect(await readClipboardImage({ platform: "win32", env: {} })).toBeNull();
		expect(mocks.getImage).not.toHaveBeenCalled();
	});

	test.each(["linux", "win32"] as const)(
		"%s: propagates native transfer errors without fallback",
		async (platform) => {
			const error = new Error("Native clipboard operation failed");
			mocks.getImage.mockImplementation(() => {
				throw error;
			});
			await expect(readClipboardImage({ platform, env: { WAYLAND_DISPLAY: "1", DISPLAY: ":0" } })).rejects.toBe(
				error,
			);
			expect(mocks.spawnSync.mock.calls.map(([name]) => name)).toEqual(platform === "linux" ? ["wl-paste"] : []);
		},
	);

	test("Termux does not read image clipboards", async () => {
		expect(await readClipboardImage({ platform: "linux", env: { TERMUX_VERSION: "0.119" } })).toBeNull();
		expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
		expect(mocks.spawnSync).not.toHaveBeenCalled();
	});
});
