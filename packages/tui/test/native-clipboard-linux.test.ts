import assert from "node:assert/strict";
import { type ChildProcessWithoutNullStreams, execFile, execFileSync, spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { once } from "node:events";
import { mkdtempSync, rmSync } from "node:fs";
import { createServer, type Socket } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, describe, it, type TestContext } from "node:test";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

const exec = promisify(execFile);
const fixtures = fileURLToPath(new URL("./fixtures/", import.meta.url));
const native = fileURLToPath(new URL("../native/linux/", import.meta.url));
const supported = process.platform === "linux" && ["arm64", "x64"].includes(process.arch);
const dependenciesAvailable =
	supported &&
	spawnSync(
		"sh",
		[
			"-c",
			"command -v cc && command -v Xvfb && command -v xclip && command -v wayland-scanner && pkg-config --exists xcb wayland-server",
		],
		{ stdio: "ignore" },
	).status === 0;

interface ReaderResult {
	ok: boolean;
	value?: string | null;
	unavailable?: boolean;
	length?: number;
	hash?: string;
	error?: string;
}

async function startServer(
	t: TestContext,
	command: string,
	args: string[],
	env: NodeJS.ProcessEnv,
): Promise<{ child: ChildProcessWithoutNullStreams; ready: string }> {
	const child = spawn(command, args, { env, stdio: "pipe" });
	t.after(async () => {
		if (child.exitCode === null && child.signalCode === null) {
			const exited = once(child, "exit");
			child.kill("SIGKILL");
			await exited;
		}
	});
	let stderr = "";
	child.stderr.on("data", (data: Buffer) => {
		stderr += data.toString();
	});
	const ready = await new Promise<string>((resolve, reject) => {
		child.once("error", reject);
		child.once("exit", (code) => reject(new Error(`${command} exited with ${code}: ${stderr}`)));
		child.stdout.once("data", (data: Buffer) => resolve(data.toString().trim()));
	});
	return { child, ready };
}

async function readClipboard(
	backend: "wayland" | "x11",
	method: string,
	env: NodeJS.ProcessEnv,
): Promise<ReaderResult> {
	const { stdout } = await exec(
		process.execPath,
		[
			join(fixtures, "clipboard-reader.cjs"),
			join(native, "prebuilds", `linux-${process.arch}`, `linux-platform-${backend}.node`),
			method,
		],
		{ env, timeout: 6000 },
	);
	return JSON.parse(stdout) as ReaderResult;
}

describe("native Linux clipboard", { skip: !dependenciesAvailable, timeout: 60000 }, () => {
	let directory: string;

	async function startWayland(t: TestContext, protocol: string, mode: string) {
		const env = {
			...process.env,
			XDG_RUNTIME_DIR: directory,
			WAYLAND_DISPLAY: "wayland-test",
			WAYLAND_SOCKET: undefined,
		};
		await startServer(t, join(directory, `server-${protocol}`), [mode], env);
		return env;
	}

	async function startX11(t: TestContext) {
		const { child, ready } = await startServer(
			t,
			"Xvfb",
			["-displayfd", "1", "-screen", "0", "640x480x24", "-nolisten", "tcp"],
			process.env,
		);
		return { child, env: { ...process.env, DISPLAY: `:${ready}` } };
	}

	before(() => {
		directory = mkdtempSync(join(tmpdir(), "pi-clipboard-test-"));
		for (const [prefix, protocol] of [
			["ext", "ext-data-control-v1.xml"],
			["wlr", "wlr-data-control-unstable-v1.xml"],
		] as const) {
			execFileSync("wayland-scanner", [
				"server-header",
				join(native, "protocol", protocol),
				join(directory, `${prefix}-server.h`),
			]);
			execFileSync("wayland-scanner", [
				"private-code",
				join(native, "protocol", protocol),
				join(directory, `${prefix}-protocol.c`),
			]);
		}
		const flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror"];
		for (const protocol of ["ext", "wlr"]) {
			execFileSync("cc", [
				...flags,
				...(protocol === "ext" ? ["-DUSE_EXT"] : []),
				"-I",
				directory,
				join(fixtures, "clipboard-wayland-server.c"),
				join(directory, "ext-protocol.c"),
				join(directory, "wlr-protocol.c"),
				"-lwayland-server",
				"-o",
				join(directory, `server-${protocol}`),
			]);
		}
		execFileSync("cc", [
			...flags,
			join(fixtures, "clipboard-x11-test.c"),
			"-lxcb",
			"-ldl",
			"-o",
			join(directory, "x11-test"),
		]);
	});

	after(() => {
		if (directory) rmSync(directory, { recursive: true, force: true });
	});

	for (const protocol of ["ext", "wlr"]) {
		it(`reads text and images using ${protocol} data control`, async (t) => {
			const env = await startWayland(t, protocol, "normal");
			assert.equal((await readClipboard("wayland", "getText", env)).value, "Wayland café");
			assert.equal(
				(await readClipboard("wayland", "getImage", env)).hash,
				createHash("sha256").update("image bytes").digest("hex"),
			);
		});

		it(`decodes ${protocol} STRING offers as Latin-1`, async (t) => {
			const env = await startWayland(t, protocol, "latin1");
			assert.equal((await readClipboard("wayland", "getText", env)).value, "café £ÿ");
		});

		it(`preserves large ${protocol} text and image transfers`, async (t) => {
			const env = await startWayland(t, protocol, "large");
			const bytes = Buffer.alloc(4 * 1024 * 1024, "x");
			for (const method of ["getText", "getImage"]) {
				const result = await readClipboard("wayland", method, env);
				assert.equal(result.ok, true, result.error);
				assert.equal(result.length, bytes.length);
				assert.equal(result.hash, createHash("sha256").update(bytes).digest("hex"));
			}
		});

		it(`distinguishes empty ${protocol} data from an absent format`, async (t) => {
			const env = await startWayland(t, protocol, "empty-data");
			assert.equal((await readClipboard("wayland", "getText", env)).value, "");
			assert.equal((await readClipboard("wayland", "getImage", env)).length, 0);
		});

		for (const mode of ["finished", "finished-after-selection"]) {
			it(`returns unavailable when ${protocol} reports ${mode} during setup`, async (t) => {
				const env = await startWayland(t, protocol, mode);
				for (const method of ["getText", "getImage"]) {
					assert.deepEqual(await readClipboard("wayland", method, env), { ok: true, unavailable: true });
				}
			});
		}
	}

	it("supports version 1 of wlr data control", async (t) => {
		const env = await startWayland(t, "wlr", "version1");
		assert.equal((await readClipboard("wayland", "getText", env)).value, "Wayland café");
	});

	it("opens a fresh named Wayland connection instead of consuming an inherited socket", async (t) => {
		const env = { ...(await startWayland(t, "ext", "normal")), WAYLAND_SOCKET: "-1" };
		assert.equal((await readClipboard("wayland", "getText", env)).value, "Wayland café");
	});

	for (const mode of ["empty", "text-only"]) {
		it(`returns null for absent Wayland images: ${mode}`, async (t) => {
			const env = await startWayland(t, "ext", mode);
			assert.deepEqual(await readClipboard("wayland", "getImage", env), { ok: true, value: null });
			if (mode === "empty")
				assert.deepEqual(await readClipboard("wayland", "getText", env), { ok: true, value: null });
		});
	}

	for (const mode of ["stall-registry", "stall-offer", "slow"]) {
		it(`bounds Wayland discovery and transfer time: ${mode}`, async (t) => {
			const env = await startWayland(t, "ext", mode);
			const started = performance.now();
			const result = await readClipboard("wayland", "getText", env);
			if (mode === "slow") {
				assert.equal(result.ok, false);
				assert.match(result.error ?? "", /Wayland clipboard/);
			} else {
				assert.deepEqual(result, { ok: true, unavailable: true });
			}
			assert.ok(performance.now() - started < 3500, "The whole operation must use one 2-second deadline");
		});
	}

	it("validates X11 incremental property metadata before accumulating bytes", () => {
		assert.equal(execFileSync(join(directory, "x11-test"), ["metadata"], { encoding: "utf8" }).trim(), "validated");
	});

	for (const method of ["getText", "getImage"]) {
		it(`bounds stalled X11 connection setup: ${method}`, async (t) => {
			const sockets = new Set<Socket>();
			let connections = 0;
			const server = createServer((socket) => {
				connections++;
				sockets.add(socket);
				socket.on("data", () => {}); // Accept setup bytes without replying.
				socket.on("close", () => sockets.delete(socket));
			});
			t.after(async () => {
				for (const socket of sockets) socket.destroy();
				await new Promise<void>((resolve, reject) => server.close((error) => (error ? reject(error) : resolve())));
			});
			server.listen(0, "127.0.0.1");
			await once(server, "listening");
			const address = server.address();
			assert.ok(address && typeof address !== "string" && address.port > 6000);
			const env = { ...process.env, DISPLAY: `127.0.0.1:${address.port - 6000}` };
			const started = performance.now();
			const result = await readClipboard("x11", method, env);
			assert.ok(performance.now() - started < 3500, "Connection setup must share the 2-second deadline");
			assert.deepEqual(result, { ok: true, unavailable: true });
			assert.ok(connections > 0, "The helper must reach the stalled X11 server");
			assert.equal(sockets.size, 0, "The timed-out child must release its X11 connection");
		});
	}

	it("decodes X11 STRING as Latin-1", async (t) => {
		const { env } = await startX11(t);
		execFileSync("xclip", ["-selection", "clipboard", "-in", "-t", "STRING"], {
			env,
			input: Buffer.from("café £ÿ", "latin1"),
			stdio: ["pipe", "ignore", "ignore"],
			timeout: 5000,
		});
		const result = await readClipboard("x11", "getText", env);
		assert.equal(result.ok, true, result.error);
		assert.equal(result.value, "café £ÿ");
	});

	it("returns null for empty X11 clipboards and text-only selections", async (t) => {
		const { env } = await startX11(t);
		assert.deepEqual(await readClipboard("x11", "getText", env), { ok: true, value: null });
		assert.deepEqual(await readClipboard("x11", "getImage", env), { ok: true, value: null });
		execFileSync("xclip", ["-selection", "clipboard", "-in", "-t", "UTF8_STRING"], {
			env,
			input: "text only",
			stdio: ["pipe", "ignore", "ignore"],
			timeout: 5000,
		});
		assert.deepEqual(await readClipboard("x11", "getImage", env), { ok: true, value: null });
	});

	it("preserves X11 Unicode and incremental text/image transfers", async (t) => {
		const { env } = await startX11(t);
		for (const [target, method, bytes] of [
			["UTF8_STRING", "getText", Buffer.from("café 日本語")],
			["UTF8_STRING", "getText", Buffer.alloc(4 * 1024 * 1024, 120)],
			["image/png", "getImage", Buffer.alloc(4 * 1024 * 1024, 123)],
		] as const) {
			execFileSync("xclip", ["-selection", "clipboard", "-in", "-t", target], {
				env,
				input: bytes,
				stdio: ["pipe", "ignore", "ignore"],
				timeout: 5000,
			});
			const result = await readClipboard("x11", method, env);
			assert.equal(result.ok, true, result.error);
			assert.equal(result.length, bytes.length);
			assert.equal(result.hash, createHash("sha256").update(bytes).digest("hex"));
		}
	});

	for (const mode of ["disconnect", "timeout"]) {
		it(`reports X11 transfer ${mode} as an exception, not an unavailable display`, async (t) => {
			const { child, env } = await startX11(t);
			const owner = await startServer(t, join(directory, "x11-test"), ["idle"], env);
			const requested = once(owner.child.stdout, "data");
			const started = performance.now();
			const result = readClipboard("x11", "getText", env);
			await requested;
			if (mode === "disconnect") child.kill("SIGKILL");
			const failure = await result;
			assert.equal(failure.ok, false);
			assert.match(failure.error ?? "", /X11 clipboard/);
			assert.ok(performance.now() - started < 3500, "The transfer must share the 2-second deadline");
		});
	}

	for (const content of ["text", "image"]) {
		it(`frees partially received X11 ${content} after invalid metadata`, async (t) => {
			const { env } = await startX11(t);
			await startServer(t, join(directory, "x11-test"), ["invalid"], env);
			const { stdout } = await exec(join(directory, "x11-test"), [content], { env, timeout: 6000 });
			assert.equal(stdout.trim(), "clean");
		});

		it(`terminates and reaps stalled X11 ${content} after a partial transfer`, async (t) => {
			const { env } = await startX11(t);
			await startServer(t, join(directory, "x11-test"), ["partial"], env);
			const started = performance.now();
			const result = await readClipboard("x11", content === "text" ? "getText" : "getImage", env);
			assert.equal(result.ok, false);
			assert.match(result.error ?? "", /X11 clipboard/);
			assert.ok(performance.now() - started < 3500);
		});
	}
});
