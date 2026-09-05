#include <xcb/xcb.h>
#include <string.h>
#define PI_CLIPBOARD_BACKEND "X11"
#include "clipboard-io.h"

typedef struct {
    unsigned char* data;
    size_t length;
    uint32_t items;
    uint8_t format;
    xcb_atom_t type;
} property_data;

typedef struct {
    xcb_connection_t* connection;
    xcb_window_t window;
    xcb_atom_t clipboard;
    xcb_atom_t property;
    xcb_atom_t targets;
    xcb_atom_t incr;
} x11_clipboard;

static bool append_bytes(property_data* result, const unsigned char* bytes, size_t length) {
    if (length > MAX_CLIPBOARD_BYTES - result->length) return false;
    unsigned char* data = realloc(result->data, result->length + length + 1);
    if (!data) return false;
    memcpy(data + result->length, bytes, length);
    result->length += length;
    data[result->length] = 0;
    result->data = data;
    return true;
}

// Blocking XCB calls are bounded by the parent's operation deadline.
static xcb_generic_event_t* wait_for_event(x11_clipboard* clipboard, uint8_t type) {
    if (xcb_flush(clipboard->connection) <= 0) return 0;
    xcb_generic_event_t* event;
    while ((event = xcb_wait_for_event(clipboard->connection))) {
        if ((event->response_type & 0x7f) == type) return event;
        free(event);
    }
    return 0;
}

static bool read_property(x11_clipboard* clipboard, bool remove, property_data* result) {
    xcb_get_property_cookie_t cookie = xcb_get_property(
        clipboard->connection, remove, clipboard->window, clipboard->property,
        XCB_GET_PROPERTY_TYPE_ANY, 0, MAX_CLIPBOARD_BYTES / 4
    );
    xcb_get_property_reply_t* reply = xcb_get_property_reply(clipboard->connection, cookie, 0);
    if (!reply) return false;
    bool copied = false;
    bool valid_format = reply->format == 8 || reply->format == 16 || reply->format == 32;
    uint64_t length = (uint64_t)reply->value_len * (reply->format / 8);
    if (reply->bytes_after == 0 && reply->type != XCB_NONE && valid_format &&
        length <= MAX_CLIPBOARD_BYTES && length <= (uint64_t)reply->length * 4) {
        result->type = reply->type;
        result->format = reply->format;
        result->items = reply->value_len;
        copied = append_bytes(result, xcb_get_property_value(reply), (size_t)length);
    }
    free(reply);
    return copied;
}

static bool append_property(property_data* result, const property_data* chunk) {
    if ((chunk->format != 8 && chunk->format != 16 && chunk->format != 32) ||
        chunk->type == XCB_NONE ||
        (uint64_t)chunk->items * (chunk->format / 8) != chunk->length ||
        (result->type != XCB_NONE && (result->type != chunk->type || result->format != chunk->format)) ||
        chunk->items > UINT32_MAX - result->items) return false;
    if (!append_bytes(result, chunk->data, chunk->length)) return false;
    result->type = chunk->type;
    result->format = chunk->format;
    result->items += chunk->items;
    return true;
}

static bool request_selection(x11_clipboard* clipboard, xcb_atom_t target, property_data* result) {
    memset(result, 0, sizeof(*result));
    xcb_delete_property(clipboard->connection, clipboard->window, clipboard->property);
    xcb_convert_selection(
        clipboard->connection, clipboard->window, clipboard->clipboard,
        target, clipboard->property, XCB_CURRENT_TIME
    );

    while (true) {
        xcb_selection_notify_event_t* event = (xcb_selection_notify_event_t*)wait_for_event(clipboard, XCB_SELECTION_NOTIFY);
        if (!event) return false;
        bool matches = event->selection == clipboard->clipboard && event->target == target;
        bool received = event->property != XCB_NONE;
        free(event);
        if (!matches) continue;
        if (!received) return true; // No owner, or the owner does not offer this target.
        if (!read_property(clipboard, false, result)) return false;
        break;
    }
    if (result->type != clipboard->incr) return true;

    free(result->data);
    memset(result, 0, sizeof(*result));
    xcb_delete_property(clipboard->connection, clipboard->window, clipboard->property);

    while (true) {
        xcb_property_notify_event_t* event = (xcb_property_notify_event_t*)wait_for_event(clipboard, XCB_PROPERTY_NOTIFY);
        if (!event) return false;
        bool matches = event->atom == clipboard->property && event->state == XCB_PROPERTY_NEW_VALUE;
        free(event);
        if (!matches) continue;

        property_data chunk = {0};
        if (!read_property(clipboard, true, &chunk)) return false;
        bool appended = append_property(result, &chunk);
        bool finished = chunk.items == 0;
        free(chunk.data);
        if (!appended) return false;
        if (finished) return true;
    }
}

static xcb_atom_t intern_atom(x11_clipboard* clipboard, const char* name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(clipboard->connection, false, strlen(name), name);
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(clipboard->connection, cookie, 0);
    if (!reply) return XCB_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

static bool open_clipboard(x11_clipboard* clipboard) {
    memset(clipboard, 0, sizeof(*clipboard));
    int screen_number = 0;
    clipboard->connection = xcb_connect(0, &screen_number);
    if (xcb_connection_has_error(clipboard->connection)) return false;
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(clipboard->connection));
    while (screen_number-- > 0 && screens.rem) xcb_screen_next(&screens);
    if (!screens.rem) return false;

    clipboard->window = xcb_generate_id(clipboard->connection);
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(
        clipboard->connection, XCB_COPY_FROM_PARENT, clipboard->window,
        screens.data->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
        XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &mask
    );
    clipboard->clipboard = intern_atom(clipboard, "CLIPBOARD");
    clipboard->property = intern_atom(clipboard, "PI_CLIPBOARD");
    clipboard->targets = intern_atom(clipboard, "TARGETS");
    clipboard->incr = intern_atom(clipboard, "INCR");
    return clipboard->clipboard && clipboard->property && clipboard->targets && clipboard->incr;
}

static void close_clipboard(x11_clipboard* clipboard) {
    if (clipboard->connection) xcb_disconnect(clipboard->connection);
}

static bool preferred_target(x11_clipboard* clipboard, bool image, xcb_atom_t* target) {
    const char* const* types = image ? image_types : text_types;
    size_t type_count = image ? 6 : 5;
    xcb_atom_t wanted[6];
    for (size_t index = 0; index < type_count; index++) {
        wanted[index] = intern_atom(clipboard, types[index]);
        if (wanted[index] == XCB_NONE) return false;
    }

    property_data targets = {0};
    bool received = request_selection(clipboard, clipboard->targets, &targets);
    bool valid = targets.type == XCB_ATOM_ATOM && targets.format == 32 &&
        targets.items == targets.length / sizeof(xcb_atom_t) && targets.length % sizeof(xcb_atom_t) == 0;
    if (received && valid) {
        xcb_atom_t* offered = (xcb_atom_t*)targets.data;
        for (size_t wanted_index = 0; wanted_index < type_count; wanted_index++) {
            for (uint32_t offered_index = 0; offered_index < targets.items; offered_index++) {
                if (offered[offered_index] == wanted[wanted_index]) {
                    free(targets.data);
                    *target = wanted[wanted_index];
                    return true;
                }
            }
        }
    }
    free(targets.data);
    return received && (valid || targets.type == XCB_NONE);
}

static bool read_selection(x11_clipboard* clipboard, bool image, property_data* result) {
    xcb_atom_t target = XCB_NONE;
    bool received = preferred_target(clipboard, image, &target) &&
        (target == XCB_NONE || request_selection(clipboard, target, result));
    if (!received) {
        free(result->data);
        memset(result, 0, sizeof(*result));
    }
    return received;
}

static bool read_clipboard(bool image, int fd, clipboard_bytes* result) {
    x11_clipboard clipboard;
    if (!open_clipboard(&clipboard) || !clipboard_opened(fd)) return false;
    property_data contents = {0};
    bool received = read_selection(&clipboard, image, &contents);
    close_clipboard(&clipboard);
    result->data = contents.data;
    result->length = contents.length;
    result->latin1 = contents.type == XCB_ATOM_STRING;
    return received;
}
