# Linux clipboard helpers

These N-API helpers provide read-only clipboard access for Linux terminals without requiring `wl-paste`, `xclip`, or `xsel` executables.

- `linux-platform-wayland.node` uses the standardized `ext-data-control-v1` protocol when available and otherwise uses `wlr-data-control-unstable-v1`. It dynamically links only `libwayland-client.so.0`.
- `linux-platform-x11.node` implements X11 selection reads, including `TARGETS` discovery and incremental (`INCR`) transfers. It dynamically links only `libxcb.so.1`. XCB reports connection errors instead of terminating the host process like Xlib.

Both backends use the same child-process runner, MIME preferences, and N-API result conversion in `src/clipboard-io.h`. Each read opens a fresh display connection in a short-lived child, with one parent-enforced two-second deadline covering setup, discovery, flushing, and data transfer. This lets the backends use blocking library calls without separate timeout loops. The parent terminates and reaps the child before returning; process exit releases the child's display objects, descriptors, and allocations. Wayland connects by display name rather than consuming the parent's `WAYLAND_SOCKET`.

The helpers intentionally do not write clipboard data. Coding-agent retains its existing `wl-copy`, `xclip`, `xsel`, Termux, and OSC 52 write paths, avoiding resident native clipboard-owner threads.

Both helpers are linked without a direct libc dependency. The same prebuild for an architecture can therefore load on glibc and musl as long as the corresponding display client library is installed. The loader caches modules without opening a display. Reads try Wayland first when `WAYLAND_DISPLAY` is set, then X11 when `DISPLAY` is set, retrying unavailable displays on subsequent reads.

TUI exposes `getNativeClipboard()`, or `getNativeClipboard("wayland" | "x11")` to select a specific Linux backend. Its synchronous `getText()` and `getImage()` methods return `undefined` when the display cannot be opened, `null` when the requested content is absent, and throw on transfer failures. Display setup, content discovery, and reading share one operation and deadline, without separate availability probes. Coding-agent tries each display's command-line reader and native reader before falling back to the next display.

The build generates temporary protocol bindings with `wayland-scanner` from the XML specifications under `protocol/`; only the specifications and handwritten implementation are checked in.

## Building

Install a C compiler, `wayland-scanner`, and the Wayland and XCB development headers, then run on Linux:

```bash
npm --prefix packages/tui run build:native:linux
```

The build produces prebuilds for the host architecture (`x64` or `arm64`). Build once on each architecture; the result does not need separate glibc and musl variants.

## Testing

From `packages/tui`, run:

```bash
node --test test/native-clipboard-linux.test.ts
```

The tests require the build dependencies above plus `pkg-config`, `Xvfb`, and `xclip`. They use isolated X11 and Wayland servers, not the desktop clipboard, and skip when these dependencies are unavailable. Coverage includes Unicode, Latin-1, large transfers on both backends, incremental metadata validation, stalled Wayland discovery and data transfers, stalled X11 connection setup, X11 disconnects, and allocation tracking for failed X11 transfers. Every native read checks that no child processes or zombies remain before the test reader exits.
