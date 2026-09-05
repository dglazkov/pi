#include <wayland-server.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ext-server.h"
#include "wlr-server.h"

#ifdef USE_EXT
#define manager ext_data_control_manager_v1
#define device ext_data_control_device_v1
#define offer ext_data_control_offer_v1
#else
#define manager zwlr_data_control_manager_v1
#define device zwlr_data_control_device_v1
#define offer zwlr_data_control_offer_v1
#endif
#define CONCAT_INNER(a, b) a##b
#define CONCAT(a, b) CONCAT_INNER(a, b)
#define INTERFACE(a) CONCAT(a, _interface)
#define SEND(a, b) CONCAT(a, b)

static const char* mode;

static void destroy_resource(struct wl_client* client, struct wl_resource* resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void receive(struct wl_client* client, struct wl_resource* resource, const char* mime, int32_t fd) {
    (void)client;
    (void)resource;
    const char* text = strcmp(mime, "image/png") == 0 ? "image bytes"
        : strcmp(mime, "STRING") == 0 ? "caf\xe9 \xa3\xff" : "Wayland café";
    size_t length = strlen(text);
    char large[4096];
    if (strcmp(mode, "large") == 0) {
        memset(large, 'x', sizeof(large));
        for (int chunk = 0; chunk < 1024; chunk++) {
            size_t offset = 0;
            while (offset < sizeof(large)) {
                ssize_t count = write(fd, large + offset, sizeof(large) - offset);
                if (count <= 0) { close(fd); return; }
                offset += (size_t)count;
            }
        }
        close(fd);
        return;
    }
    if (strcmp(mode, "empty-data") == 0) length = 0;
    bool slow = strcmp(mode, "slow") == 0;
    while (length) {
        if (slow) {
            struct timespec delay = {0, 400000000};
            nanosleep(&delay, 0);
        }
        ssize_t count = write(fd, text, slow ? 1 : length);
        if (count <= 0) break;
        text += count;
        length -= count;
    }
    close(fd);
}

static const struct INTERFACE(offer) offer_impl = { .receive = receive, .destroy = destroy_resource };
static const struct INTERFACE(device) device_impl = { .destroy = destroy_resource };

static void get_data_device(struct wl_client* client, struct wl_resource* resource, uint32_t id, struct wl_resource* seat) {
    (void)seat;
    if (strcmp(mode, "stall-offer") == 0) sleep(10);
    struct wl_resource* dev = wl_resource_create(client, &INTERFACE(device), wl_resource_get_version(resource), id);
    wl_resource_set_implementation(dev, &device_impl, 0, 0);
    if (strcmp(mode, "finished") == 0) {
        SEND(device, _send_finished)(dev);
        return;
    }
    if (strcmp(mode, "empty") == 0) {
        SEND(device, _send_selection)(dev, 0);
        return;
    }
    struct wl_resource* data = wl_resource_create(client, &INTERFACE(offer), 1, 0);
    wl_resource_set_implementation(data, &offer_impl, 0, 0);
    SEND(device, _send_data_offer)(dev, data);
    SEND(offer, _send_offer)(data, "STRING");
    if (strcmp(mode, "latin1") != 0) SEND(offer, _send_offer)(data, "text/plain;charset=utf-8");
    if (strcmp(mode, "text-only") != 0) SEND(offer, _send_offer)(data, "image/png");
    SEND(device, _send_selection)(dev, data);
    if (strcmp(mode, "finished-after-selection") == 0) SEND(device, _send_finished)(dev);
}

static const struct INTERFACE(manager) manager_impl = { .get_data_device = get_data_device, .destroy = destroy_resource };

static void bind_manager(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource* resource = wl_resource_create(client, &INTERFACE(manager), version, id);
    wl_resource_set_implementation(resource, &manager_impl, 0, 0);
}

static void bind_seat(struct wl_client* client, void* data, uint32_t version, uint32_t id) {
    (void)data;
    if (strcmp(mode, "slow") == 0) sleep(1);
    struct wl_resource* resource = wl_resource_create(client, &wl_seat_interface, version, id);
    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_KEYBOARD);
    if (version >= 2) wl_seat_send_name(resource, "seat0");
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    mode = argv[1];
    signal(SIGPIPE, SIG_IGN);
    struct wl_display* display = wl_display_create();
    wl_global_create(display, &wl_seat_interface, 2, 0, bind_seat);
    uint32_t version = strcmp(mode, "version1") == 0 ? 1 : INTERFACE(manager).version;
    wl_global_create(display, &INTERFACE(manager), version, 0, bind_manager);
    if (wl_display_add_socket(display, "wayland-test") != 0) return 1;
    puts("ready");
    fflush(stdout);
    if (strcmp(mode, "stall-registry") == 0) sleep(10);
    wl_display_run(display);
    wl_display_destroy(display);
}
