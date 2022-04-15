#include "utils.h"
#include "extensions/xdg-shell-client-protocol.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

struct wayland_client
{
    // Global
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct wl_compositor *compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_seat *seat;
    // Objects
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_pointer *pointer;
    struct wl_surface *cursor_surface;
    struct wl_cursor_image *cursor_image;
    struct wl_keyboard *keyboard;
    struct xkb_context *xkb_context;
    struct xkb_state *xkb_state;
    // Stored values
    int32_t width;
    int32_t height;
    wl_fixed_t pointer_x_position;
    wl_fixed_t pointer_y_position;
    bool should_close;
};

// ####################################################################################################################
// Helpers

static void client_load_cursor(struct wayland_client *client, char *cursor_name)
{
    struct wl_cursor_theme *cursor_theme = wl_cursor_theme_load(NULL, 24, client->shm);
    struct wl_cursor *cursor = wl_cursor_theme_get_cursor(cursor_theme, cursor_name);
    client->cursor_image = cursor->images[0];
    struct wl_buffer *cursor_buffer = wl_cursor_image_get_buffer(client->cursor_image);
    client->cursor_surface = wl_compositor_create_surface(client->compositor);
    wl_surface_attach(client->cursor_surface, cursor_buffer, 0, 0);
    wl_surface_commit(client->cursor_surface);
}

// ####################################################################################################################
// Buffer

static void wl_buffer_release(void *data, struct wl_buffer *buffer)
{
    wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = wl_buffer_release,
};

static struct wl_buffer *buffer_draw(struct wayland_client *client)
{
    int32_t width = client->width;
    int32_t height = client->height;
    int32_t stride = width * 4;
    int32_t size = stride * height;

    int fd = allocate_shm_file(size);

    if (fd == -1)
    {
        return NULL;
    }

    uint32_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (data == MAP_FAILED)
    {
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(client->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            data[y * width + x] = 0xff241f31;
        }
    }

    munmap(data, size);
    wl_buffer_add_listener(buffer, &buffer_listener, NULL);

    return buffer;
}

// ####################################################################################################################
// XDG Toplevel

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height, struct wl_array *states)
{
    struct wayland_client *client = data;

    if (width == 0 || height == 0)
    {
        return;
    }

    client->width = width;
    client->height = height;
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    struct wayland_client *client = data;
    client->should_close = true;
}

static void xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
    .configure_bounds = xdg_toplevel_configure_bounds,
};

// ####################################################################################################################
// XDG Surface

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct wayland_client *client = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    struct wl_buffer *buffer = buffer_draw(client);
    wl_surface_attach(client->surface, buffer, 0, 0);
    wl_surface_commit(client->surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

// ####################################################################################################################
// XDG WM Base

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

// ####################################################################################################################
// Keyboard

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
    assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
    struct wayland_client *client = data;

    char *map_shm = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(map_shm != MAP_FAILED);

    struct xkb_keymap *keymap = xkb_keymap_new_from_string(client->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_shm, size);
    close(fd);

    client->xkb_state = xkb_state_new(keymap);

    // FIXME: Unref keymap. But when? (Wayland Book Ch. 9.2)
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t key_state)
{
    struct wayland_client *client = data;
    xkb_keysym_t xkb_key = xkb_state_key_get_one_sym(client->xkb_state, key + 8);

    if (xkb_key == XKB_KEY_Escape)
    {
        client->should_close = true;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    struct wayland_client *client = data;
    xkb_state_update_mask(client->xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
}

static struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

// ####################################################################################################################
// Pointer

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t x_position, wl_fixed_t y_position)
{
    struct wayland_client *client = data;
    wl_pointer_set_cursor(client->pointer, serial, client->cursor_surface, client->cursor_image->hotspot_x, client->cursor_image->hotspot_y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x_position, wl_fixed_t y_position)
{
    struct wayland_client *client = data;
    client->pointer_x_position = x_position;
    client->pointer_y_position = y_position;
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    struct wayland_client *client = data;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
    {
        // Handle move and resize.

        const uint32_t drag_border_width = 20;

        const int32_t pointer_x_position = wl_fixed_to_int(client->pointer_x_position);
        const int32_t pointer_y_position = wl_fixed_to_int(client->pointer_y_position);

        const bool pointer_is_left = pointer_x_position < drag_border_width;
        const bool pointer_is_top = pointer_y_position < drag_border_width;
        const bool pointer_is_right = client->width - pointer_x_position < drag_border_width;
        const bool pointer_is_bottom = client->height - pointer_y_position < drag_border_width;

        if (pointer_is_left && pointer_is_top)
        {
            xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT);
        }
        else if (pointer_is_right && pointer_is_top)
        {
            xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT);
        }
        else if (pointer_is_left && pointer_is_bottom)
        {
            xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT);
        }
        else if (pointer_is_right && pointer_is_bottom)
        {
            xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
        }
        else if (pointer_is_left)
        {
            xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_LEFT);
        }
        else if (pointer_is_top)
        {
            xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_TOP);
        }
        else if (pointer_is_right)
        {
            xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_RIGHT);
        }
        else if (pointer_is_bottom)
        {
            xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM);
        }
        else
        {
            xdg_toplevel_move(client->xdg_toplevel, client->seat, serial);
        }
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source)
{
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis)
{
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

// ####################################################################################################################
// Seat

static void wl_seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct wayland_client *client = data;

    bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    bool have_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;

    if (have_pointer && client->pointer == NULL)
    {
        client->pointer = wl_seat_get_pointer(client->seat);
        wl_pointer_add_listener(client->pointer, &pointer_listener, client);
    }
    else
    {
        // TODO Handle touch input?
    }

    if (have_keyboard)
    {
        client->keyboard = wl_seat_get_keyboard(client->seat);
        client->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        wl_keyboard_add_listener(client->keyboard, &keyboard_listener, client);
    }
    else
    {
        // What?
    }
}

static void wl_seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = wl_seat_capabilities,
    .name = wl_seat_name,
};

// ####################################################################################################################
// Registry

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    struct wayland_client *client = data;
    //printf("info (wayland): Registred interface `%s-%d`.\n", interface, version);

    if (strcmp(interface, wl_shm_interface.name) == 0)
    {
        client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    }
    else if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
        client->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 5);
    }
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
    {
        client->xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 4);
        xdg_wm_base_add_listener(client->xdg_wm_base, &xdg_wm_base_listener, client);
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
        client->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
        wl_seat_add_listener(client->seat, &seat_listener, client);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    // TODO: I have no idea what to do here.
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main()
{
    struct wayland_client client = {0};
    client.width = 1280;
    client.height = 720;

    client.display = wl_display_connect(NULL);
    client.registry = wl_display_get_registry(client.display);
    wl_registry_add_listener(client.registry, &registry_listener, &client);
    wl_display_roundtrip(client.display);

    client.surface = wl_compositor_create_surface(client.compositor);
    client.xdg_surface = xdg_wm_base_get_xdg_surface(client.xdg_wm_base, client.surface);
    xdg_surface_add_listener(client.xdg_surface, &xdg_surface_listener, &client);
    client.xdg_toplevel = xdg_surface_get_toplevel(client.xdg_surface);
    xdg_toplevel_add_listener(client.xdg_toplevel, &xdg_toplevel_listener, &client);
    xdg_toplevel_set_title(client.xdg_toplevel, "Minimal Window");
    xdg_toplevel_set_min_size(client.xdg_toplevel, 300, 300);
    wl_surface_commit(client.surface);

    client_load_cursor(&client, "left_ptr");

    printf("Use the Escape key to close the window.\n");

    while (wl_display_dispatch(client.display) != -1 && !client.should_close)
    {
    }
}
