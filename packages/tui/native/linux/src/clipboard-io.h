#ifndef PI_CLIPBOARD_IO_H
#define PI_CLIPBOARD_IO_H

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../napi.h"

#define MAX_CLIPBOARD_BYTES (50u * 1024u * 1024u)
#define CLIPBOARD_TIMEOUT_MS 2000

typedef enum { CLIPBOARD_UNAVAILABLE, CLIPBOARD_FAILED, CLIPBOARD_READ } clipboard_status;

typedef struct {
    unsigned char* data;
    size_t length;
    bool latin1;
} clipboard_bytes;

static const char* const text_types[] = {
    "text/plain;charset=utf-8", "text/plain;charset=UTF-8", "UTF8_STRING", "text/plain", "STRING", 0,
};
static const char* const image_types[] = {
    "image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp", "image/tiff", 0,
};

// Backends run only in the child. Report successful setup before reading data
// so a transfer timeout is distinguishable from an unavailable display.
static bool read_clipboard(bool image, int fd, clipboard_bytes* result);

static bool clipboard_opened(int fd) {
    unsigned char opened = 1;
    ssize_t count;
    do { count = send(fd, &opened, 1, MSG_NOSIGNAL); } while (count < 0 && errno == EINTR);
    return count == 1;
}

static int64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// Reuse the operation's deadline: neither incoming chunks nor signals extend it.
static int wait_for_fd(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t remaining = deadline - monotonic_ms();
        if (remaining <= 0) return 0;
        struct pollfd descriptor = {fd, events, 0};
        int ready = poll(&descriptor, 1, (int)remaining);
        if (ready >= 0) return ready ? descriptor.revents : 0;
        if (errno != EINTR) return 0;
    }
}

static bool transfer_bytes(int fd, void* buffer, size_t length, bool sending, int64_t deadline) {
    unsigned char* cursor = buffer;
    while (length) {
        short events = sending ? POLLOUT : POLLIN;
        if (!(wait_for_fd(fd, events, deadline) & events)) return false;
        ssize_t count = sending ? send(fd, cursor, length, MSG_NOSIGNAL) : recv(fd, cursor, length, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return false;
        cursor += count;
        length -= (size_t)count;
    }
    return true;
}

// Display libraries can block during connection setup, flushing, or reads.
// One parent-enforced deadline covers all of them, allowing ordinary blocking
// APIs in the child. Never call Node APIs there. Process exit releases its
// display objects, file descriptors, and allocations, even after a timeout.
static clipboard_status run_clipboard_operation(bool image, clipboard_bytes* result) {
    int64_t deadline = monotonic_ms() + CLIPBOARD_TIMEOUT_MS;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets) != 0) return CLIPBOARD_UNAVAILABLE;
    struct {
        size_t length;
        bool present;
        bool latin1;
    } response = {0};
    pid_t child = fork();
    if (child == 0) {
        close(sockets[0]);
        clipboard_bytes contents = {0};
        bool success = read_clipboard(image, sockets[1], &contents);
        if (success) {
            response.length = contents.length;
            response.present = contents.data != 0;
            response.latin1 = contents.latin1;
            success = transfer_bytes(sockets[1], &response, sizeof(response), true, deadline) &&
                transfer_bytes(sockets[1], contents.data, contents.length, true, deadline);
        }
        _exit(success ? 0 : 1);
    }
    close(sockets[1]);
    if (child < 0) {
        close(sockets[0]);
        return CLIPBOARD_UNAVAILABLE;
    }

    unsigned char opened = 0;
    bool received = transfer_bytes(sockets[0], &opened, 1, false, deadline) && opened &&
        transfer_bytes(sockets[0], &response, sizeof(response), false, deadline) &&
        response.length <= MAX_CLIPBOARD_BYTES;
    if (received && response.present) {
        result->data = malloc(response.length + 1);
        received = result->data && transfer_bytes(sockets[0], result->data, response.length, false, deadline);
        if (received) {
            result->length = response.length;
            result->data[result->length] = 0;
            result->latin1 = response.latin1;
        }
    }
    close(sockets[0]);
    // A complete response is sufficient: do not wait for child cleanup.
    kill(child, SIGKILL);
    while (waitpid(child, 0, 0) < 0 && errno == EINTR) {}
    if (!received) {
        free(result->data);
        *result = (clipboard_bytes){0};
    }
    return !opened ? CLIPBOARD_UNAVAILABLE : received ? CLIPBOARD_READ : CLIPBOARD_FAILED;
}

static napi_value get_clipboard(napi_env env, bool image) {
    clipboard_bytes contents = {0};
    clipboard_status read = run_clipboard_operation(image, &contents);
    if (read == CLIPBOARD_UNAVAILABLE) return undefined_value(env);
    if (read == CLIPBOARD_FAILED) return fail(env, "Could not read " PI_CLIPBOARD_BACKEND " clipboard");
    if (!contents.data) return null_value(env);

    napi_value result = 0;
    int status = 1;
    if (image) {
        napi_create_buffer_copy_fn create_buffer = (napi_create_buffer_copy_fn)node_symbol("napi_create_buffer_copy");
        if (create_buffer) status = create_buffer(env, contents.length, contents.data, 0, &result);
    } else {
        // The X11 compatibility target STRING is Latin-1 on both backends.
        napi_create_string_utf8_fn create_string = (napi_create_string_utf8_fn)node_symbol(
            contents.latin1 ? "napi_create_string_latin1" : "napi_create_string_utf8"
        );
        if (create_string) status = create_string(env, (const char*)contents.data, contents.length, &result);
    }
    free(contents.data);
    return status == 0 ? result : fail(env, "Could not create clipboard value");
}

static napi_value get_clipboard_text(napi_env env, napi_callback_info info) {
    (void)info;
    return get_clipboard(env, false);
}

static napi_value get_clipboard_image(napi_env env, napi_callback_info info) {
    (void)info;
    return get_clipboard(env, true);
}

PI_NAPI_EXPORT napi_value napi_register_module_v1(napi_env env, napi_value exports) {
    set_function_export(env, exports, "getText", get_clipboard_text);
    set_function_export(env, exports, "getImage", get_clipboard_image);
    return exports;
}

#endif
