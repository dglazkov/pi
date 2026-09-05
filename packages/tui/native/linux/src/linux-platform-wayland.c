#include <string.h>
#include <wayland-client.h>
#define PI_CLIPBOARD_BACKEND "Wayland"
#include "clipboard-io.h"
#include "ext-data-control-client-protocol.h"
#include "wlr-data-control-client-protocol.h"

#define MAX_OFFERS 8

typedef struct {
    void* proxy;
    const char* const* types;
    size_t rank;
} offer_info;

typedef struct {
    struct wl_display* display;
    struct wl_registry* registry;
    struct wl_seat* seat;
    struct ext_data_control_manager_v1* ext_manager;
    struct zwlr_data_control_manager_v1* wlr_manager;
    bool finished;
    const char* const* types;
    offer_info offers[MAX_OFFERS];
    size_t offer_count;
    offer_info* selection;
} clipboard_state;

static void record_mime(offer_info* info, const char* mime) {
    for (size_t index = 0; info->types[index] && index < info->rank; index++) {
        if (strcmp(mime, info->types[index]) == 0) {
            info->rank = index;
            break;
        }
    }
}

static offer_info* find_offer(clipboard_state* state, void* proxy) {
    for (size_t index = 0; index < state->offer_count; index++) {
        if (state->offers[index].proxy == proxy) return &state->offers[index];
    }
    return 0;
}

static offer_info* add_offer(clipboard_state* state, void* proxy) {
    if (state->offer_count >= MAX_OFFERS) return 0;
    offer_info* info = &state->offers[state->offer_count++];
    info->proxy = proxy;
    info->types = state->types;
    info->rank = SIZE_MAX;
    return info;
}

// Generate only the typed listener adapters. Both protocols share the same
// selection logic; requests still use their scanner-generated bindings.
#define DATA_CONTROL_LISTENERS(prefix) \
    static void prefix##_mime(void* data, struct prefix##_offer_v1* offer, const char* mime) { \
        (void)offer; \
        record_mime(data, mime); \
    } \
    static const struct prefix##_offer_v1_listener prefix##_offer_listener = {prefix##_mime}; \
    static void prefix##_data_offer(void* data, struct prefix##_device_v1* device, struct prefix##_offer_v1* offer) { \
        (void)device; \
        offer_info* info = add_offer(data, offer); \
        if (info) prefix##_offer_v1_add_listener(offer, &prefix##_offer_listener, info); \
    } \
    static void prefix##_selection(void* data, struct prefix##_device_v1* device, struct prefix##_offer_v1* offer) { \
        (void)device; \
        clipboard_state* state = data; \
        state->selection = offer ? find_offer(state, offer) : 0; \
    } \
    static void prefix##_finished(void* data, struct prefix##_device_v1* device) { \
        (void)device; \
        ((clipboard_state*)data)->finished = true; \
    } \
    static void prefix##_primary(void* data, struct prefix##_device_v1* device, struct prefix##_offer_v1* offer) { \
        (void)data; (void)device; (void)offer; \
    } \
    static const struct prefix##_device_v1_listener prefix##_device_listener = { \
        prefix##_data_offer, prefix##_selection, prefix##_finished, prefix##_primary, \
    };

DATA_CONTROL_LISTENERS(ext_data_control)
DATA_CONTROL_LISTENERS(zwlr_data_control)
#undef DATA_CONTROL_LISTENERS

static void registry_global(
    void* data,
    struct wl_registry* registry,
    uint32_t name,
    const char* interface,
    uint32_t version
) {
    clipboard_state* state = data;
    if (!state->seat && strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 2 ? version : 2);
    } else if (!state->ext_manager && strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
        state->ext_manager = wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1);
    } else if (!state->wlr_manager && strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0) {
        state->wlr_manager = wl_registry_bind(
            registry,
            name,
            &zwlr_data_control_manager_v1_interface,
            version < 2 ? version : 2
        );
    }
}

static void registry_global_remove(void* data, struct wl_registry* registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {registry_global, registry_global_remove};

static bool open_clipboard(clipboard_state* state, bool image) {
    memset(state, 0, sizeof(*state));
    state->types = image ? image_types : text_types;
    // Each child needs its own connection, not the parent's WAYLAND_SOCKET.
    unsetenv("WAYLAND_SOCKET");
    state->display = wl_display_connect(0);
    if (!state->display) return false;
    state->registry = wl_display_get_registry(state->display);
    if (!state->registry || wl_registry_add_listener(state->registry, &registry_listener, state) != 0 ||
        wl_display_roundtrip(state->display) < 0 || !state->seat || (!state->ext_manager && !state->wlr_manager)) {
        return false;
    }

    if (state->ext_manager) {
        struct ext_data_control_device_v1* device =
            ext_data_control_manager_v1_get_data_device(state->ext_manager, state->seat);
        if (!device || ext_data_control_device_v1_add_listener(device, &ext_data_control_device_listener, state) != 0) {
            return false;
        }
    } else {
        struct zwlr_data_control_device_v1* device =
            zwlr_data_control_manager_v1_get_data_device(state->wlr_manager, state->seat);
        if (!device || zwlr_data_control_device_v1_add_listener(device, &zwlr_data_control_device_listener, state) != 0) {
            return false;
        }
    }

    // A finished device is unavailable, even if it advertised a selection first.
    return wl_display_roundtrip(state->display) >= 0 && !state->finished;
}

static bool receive_offer(
    clipboard_state* state,
    offer_info* offer,
    const char* mime,
    clipboard_bytes* result
) {
    int descriptors[2];
    if (pipe(descriptors) != 0) return false;

    if (state->ext_manager != 0) {
        ext_data_control_offer_v1_receive(offer->proxy, mime, descriptors[1]);
    } else {
        zwlr_data_control_offer_v1_receive(offer->proxy, mime, descriptors[1]);
    }
    while (wl_display_flush(state->display) < 0) {
        struct pollfd descriptor = {wl_display_get_fd(state->display), POLLOUT, 0};
        if (errno == EAGAIN) {
            int ready;
            do { ready = poll(&descriptor, 1, -1); } while (ready < 0 && errno == EINTR);
            if (ready > 0) continue;
        }
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    close(descriptors[1]);

    size_t capacity = 16384;
    unsigned char* bytes = malloc(capacity);
    if (!bytes) {
        close(descriptors[0]);
        return false;
    }

    size_t length = 0;
    bool complete = false;
    while (length < MAX_CLIPBOARD_BYTES) {
        if (length == capacity) {
            size_t next_capacity = capacity * 2;
            if (next_capacity > MAX_CLIPBOARD_BYTES) next_capacity = MAX_CLIPBOARD_BYTES;
            unsigned char* next = realloc(bytes, next_capacity);
            if (!next) break;
            bytes = next;
            capacity = next_capacity;
        }

        ssize_t count = read(descriptors[0], bytes + length, capacity - length);
        if (count > 0) {
            length += (size_t)count;
        } else if (count == 0) {
            complete = true;
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    close(descriptors[0]);

    if (!complete) {
        free(bytes);
        return false;
    }
    result->data = bytes;
    result->length = length;
    result->latin1 = strcmp(mime, "STRING") == 0;
    return true;
}

static bool read_clipboard(bool image, int fd, clipboard_bytes* result) {
    clipboard_state state;
    if (!open_clipboard(&state, image) || !clipboard_opened(fd)) return false;
    offer_info* offer = state.selection;
    // An open clipboard without this format is empty, not a failed transfer.
    return !offer || offer->rank == SIZE_MAX || receive_offer(&state, offer, state.types[offer->rank], result);
}
