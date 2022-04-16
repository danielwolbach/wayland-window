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

const uint32_t BORDER_WIDTH = 5;
const uint32_t TITLEBAR_WIDTH = 30;

enum cursor_decor_position
{
    CURSOR_DECOR_POSITION_TOP_BORDER = 1u << 0,
    CURSOR_DECOR_POSITION_BOTTOM_BORDER = 1u << 1,
    CURSOR_DECOR_POSITION_LEFT_BORDER = 1u << 2,
    CURSOR_DECOR_POSITION_RIGHT_BORDER = 1u << 3,
    CURSOR_DECOR_POSITION_TOP_LEFT_CORNER = 1u << 4,
    CURSOR_DECOR_POSITION_TOP_RIGHT_CORNER = 1u << 5,
    CURSOR_DECOR_POSITION_BOTTOM_LEFT_CORNER = 1u << 6,
    CURSOR_DECOR_POSITION_BOTTOM_RIGHT_CORNER = 1u << 7,
    CURSOR_DECOR_POSITION_TITLEBAR = 1u << 8,
    CURSOR_DECOR_POSITION_CLOSE_BUTTON = 1u << 9,
};

enum cursor_variant
{
    CURSOR_VARIANT_LEFT_PTR,
    CURSOR_VARIANT_POINTER,
    CURSOR_VARIANT_N_RESIZE,
    CURSOR_VARIANT_S_RESIZE,
    CURSOR_VARIANT_W_RESIZE,
    CURSOR_VARIANT_E_RESIZE,
    CURSOR_VARIANT_NW_RESIZE,
    CURSOR_VARIANT_NE_RESIZE,
    CURSOR_VARIANT_SW_RESIZE,
    CURSOR_VARIANT_SE_RESIZE,
    CURSOR_VARIANT_COUNT,
};

struct wayland_client
{
    // Global
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct wl_compositor *compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_seat *seat;
    struct wl_subcompositor *subcompositor;
    // Objects
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_pointer *pointer;
    struct wl_surface *cursor_surface;
    struct wl_cursor_image *cursor_images[CURSOR_VARIANT_COUNT];
    struct wl_buffer *cursor_buffers[CURSOR_VARIANT_COUNT];
    struct wl_keyboard *keyboard;
    struct xkb_context *xkb_context;
    struct xkb_state *xkb_state;
    // Only decor
    struct
    {
        struct wl_surface *titlebar_surface;
        struct wl_subsurface *titlebar_subsurface;
        struct wl_surface *close_button_surface;
        struct wl_subsurface *close_button_subsurface;
        struct wl_surface *border_top_surface;
        struct wl_subsurface *border_top_subsurface;
        struct wl_surface *border_bottom_surface;
        struct wl_subsurface *border_bottom_subsurface;
        struct wl_surface *border_left_surface;
        struct wl_subsurface *border_left_subsurface;
        struct wl_surface *border_right_surface;
        struct wl_subsurface *border_right_subsurface;
        struct wl_surface *corner_top_left_surface;
        struct wl_subsurface *corner_top_left_subsurface;
        struct wl_surface *corner_top_right_surface;
        struct wl_subsurface *corner_top_right_subsurface;
        struct wl_surface *corner_bottom_left_surface;
        struct wl_subsurface *corner_bottom_left_subsurface;
        struct wl_surface *corner_bottom_right_surface;
        struct wl_subsurface *corner_bottom_right_subsurface;
    } decor;



    // Stored values
    int32_t width;
    int32_t height;
    wl_fixed_t pointer_x_position;
    wl_fixed_t pointer_y_position;
    bool should_close;
    enum cursor_decor_position cursor_decor_position;
};

// ####################################################################################################################
// Helpers

// ####################################################################################################################
// Buffer

static void wl_buffer_release(void *data, struct wl_buffer *buffer)
{
    wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = wl_buffer_release,
};

static struct wl_buffer *buffer_draw(struct wl_shm *shm, int32_t width, int32_t height, uint32_t color)
{
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

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            data[y * width + x] = color;
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

    uint32_t border_color = 0xffaaaaaa;

    // Titlebar

    struct wl_buffer *decor_buffer_titlebar = buffer_draw(client->shm, client->width - 2 * BORDER_WIDTH, TITLEBAR_WIDTH, 0xff666666);

    wl_surface_attach(client->decor.titlebar_surface, decor_buffer_titlebar, 0, 0);
    wl_subsurface_set_position(client->decor.titlebar_subsurface, 0, -TITLEBAR_WIDTH);
    wl_surface_commit(client->decor.titlebar_surface);

    // Close Button

    const uint32_t close_button_width = 20;
    const uint32_t close_button_height = 20;
    struct wl_buffer *decor_buffer_close_button = buffer_draw(client->shm, close_button_width, close_button_height, 0xffdd6666);

    wl_surface_attach(client->decor.close_button_surface, decor_buffer_close_button, 0, 0);
    wl_subsurface_set_position(client->decor.close_button_subsurface, client->width - 2 * BORDER_WIDTH - close_button_width - ((TITLEBAR_WIDTH - close_button_height) / 2.0), -((float) TITLEBAR_WIDTH / 2.0 + (float) close_button_height / 2.0));
    wl_surface_commit(client->decor.close_button_surface);

    // Edge decor

    struct wl_buffer *decor_buffer_top_bottom = buffer_draw(client->shm, client->width - 2 * BORDER_WIDTH, BORDER_WIDTH, border_color);

    wl_surface_attach(client->decor.border_top_surface, decor_buffer_top_bottom, 0, 0);
    wl_subsurface_set_position(client->decor.border_top_subsurface, 0, -BORDER_WIDTH - TITLEBAR_WIDTH);
    wl_surface_commit(client->decor.border_top_surface);

    wl_surface_attach(client->decor.border_bottom_surface, decor_buffer_top_bottom, 0, 0);
    wl_subsurface_set_position(client->decor.border_bottom_subsurface, 0, client->height - TITLEBAR_WIDTH - 2 * BORDER_WIDTH);
    wl_surface_commit(client->decor.border_bottom_surface);

    struct wl_buffer *decor_buffer_left_right = buffer_draw(client->shm, BORDER_WIDTH, client->height - 2 * BORDER_WIDTH, border_color);

    wl_surface_attach(client->decor.border_left_surface, decor_buffer_left_right, 0, 0);
    wl_subsurface_set_position(client->decor.border_left_subsurface, -BORDER_WIDTH, -TITLEBAR_WIDTH);
    wl_surface_commit(client->decor.border_left_surface);

    wl_surface_attach(client->decor.border_right_surface, decor_buffer_left_right, 0, 0);
    wl_subsurface_set_position(client->decor.border_right_subsurface, client->width - 2 * BORDER_WIDTH, -TITLEBAR_WIDTH);
    wl_surface_commit(client->decor.border_right_surface);

    // Corner decor

    struct wl_buffer *decor_cornor = buffer_draw(client->shm, BORDER_WIDTH, BORDER_WIDTH, border_color);

    wl_surface_attach(client->decor.corner_top_left_surface, decor_cornor, 0, 0);
    wl_subsurface_set_position(client->decor.corner_top_left_subsurface, -BORDER_WIDTH, -BORDER_WIDTH - TITLEBAR_WIDTH);
    wl_surface_commit(client->decor.corner_top_left_surface);

    wl_surface_attach(client->decor.corner_top_right_surface, decor_cornor, 0, 0);
    wl_subsurface_set_position(client->decor.corner_top_right_subsurface, client->width - 2 * BORDER_WIDTH, -BORDER_WIDTH - TITLEBAR_WIDTH);
    wl_surface_commit(client->decor.corner_top_right_surface);

    wl_surface_attach(client->decor.corner_bottom_left_surface, decor_cornor, 0, 0);
    wl_subsurface_set_position(client->decor.corner_bottom_left_subsurface, -BORDER_WIDTH, client->height - TITLEBAR_WIDTH - 2 * BORDER_WIDTH);
    wl_surface_commit(client->decor.corner_bottom_left_surface);

    wl_surface_attach(client->decor.corner_bottom_right_surface, decor_cornor, 0, 0);
    wl_subsurface_set_position(client->decor.corner_bottom_right_subsurface, client->width - 2 * BORDER_WIDTH, client->height - TITLEBAR_WIDTH - 2 * BORDER_WIDTH);
    wl_surface_commit(client->decor.corner_bottom_right_surface);

    // Fill window

    struct wl_buffer *buffer = buffer_draw(client->shm, client->width - 2 * BORDER_WIDTH, client->height - TITLEBAR_WIDTH - 2 * BORDER_WIDTH, 0xff444444);
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

static void set_cursor(struct wayland_client *client, uint32_t serial, enum cursor_variant cursor_variant)
{
    wl_surface_attach(client->cursor_surface, client->cursor_buffers[cursor_variant], 0, 0);
    wl_surface_commit(client->cursor_surface);
    wl_pointer_set_cursor(client->pointer, serial, client->cursor_surface, client->cursor_images[cursor_variant]->hotspot_x, client->cursor_images[cursor_variant]->hotspot_y);
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t x_position, wl_fixed_t y_position)
{
    struct wayland_client *client = data;

    if (surface == client->decor.titlebar_surface)
    {
        client->cursor_decor_position |= CURSOR_DECOR_POSITION_TITLEBAR;
        set_cursor(client, serial, CURSOR_VARIANT_LEFT_PTR);
    }
    else if (surface == client->decor.close_button_surface)
    {
        client->cursor_decor_position |= CURSOR_DECOR_POSITION_CLOSE_BUTTON;
        set_cursor(client, serial, CURSOR_VARIANT_POINTER);
    }
    else if (surface == client->decor.corner_top_left_surface)
    {
        client->cursor_decor_position |= CURSOR_DECOR_POSITION_TOP_LEFT_CORNER;
        set_cursor(client, serial, CURSOR_VARIANT_NW_RESIZE);
    }
    else if (surface == client->decor.corner_top_right_surface)
    {
        client->cursor_decor_position |= CURSOR_DECOR_POSITION_TOP_RIGHT_CORNER;
        set_cursor(client, serial, CURSOR_VARIANT_NE_RESIZE);
    }
    else if (surface == client->decor.corner_bottom_left_surface)
    {
        client->cursor_decor_position |= CURSOR_DECOR_POSITION_BOTTOM_LEFT_CORNER;
        set_cursor(client, serial, CURSOR_VARIANT_SW_RESIZE);
    }
    else if (surface == client->decor.corner_bottom_right_surface)
    {
        client->cursor_decor_position |= CURSOR_DECOR_POSITION_BOTTOM_RIGHT_CORNER;
        set_cursor(client, serial, CURSOR_VARIANT_SE_RESIZE);
    }
    else if (surface == client->decor.border_top_surface)
    {
        client->cursor_decor_position |= CURSOR_DECOR_POSITION_TOP_BORDER;
        set_cursor(client, serial, CURSOR_VARIANT_N_RESIZE);
    }
    else if (surface == client->decor.border_bottom_surface)
    {
        client->cursor_decor_position |= CURSOR_DECOR_POSITION_BOTTOM_BORDER;
        set_cursor(client, serial, CURSOR_VARIANT_S_RESIZE);
    }
    else if (surface == client->decor.border_left_surface)
    {
        client->cursor_decor_position |= CURSOR_DECOR_POSITION_LEFT_BORDER;
        set_cursor(client, serial, CURSOR_VARIANT_W_RESIZE);
    }
    else if (surface == client->decor.border_right_surface)
    {
        client->cursor_decor_position |= CURSOR_DECOR_POSITION_RIGHT_BORDER;
        set_cursor(client, serial, CURSOR_VARIANT_E_RESIZE);
    }
    else
    {
        set_cursor(client, serial, CURSOR_VARIANT_LEFT_PTR);
    }
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
    struct wayland_client *client = data;

    if (surface == client->decor.titlebar_surface)
    {
        client->cursor_decor_position &= ~CURSOR_DECOR_POSITION_TITLEBAR;
    }
    if (surface == client->decor.close_button_surface)
    {
        client->cursor_decor_position &= ~CURSOR_DECOR_POSITION_CLOSE_BUTTON;
    }
    else if (surface == client->decor.corner_top_left_surface)
    {
        client->cursor_decor_position &= ~CURSOR_DECOR_POSITION_TOP_LEFT_CORNER;
    }
    else if (surface == client->decor.corner_top_right_surface)
    {
        client->cursor_decor_position &= ~CURSOR_DECOR_POSITION_TOP_RIGHT_CORNER;
    }
    else if (surface == client->decor.corner_bottom_left_surface)
    {
        client->cursor_decor_position &= ~CURSOR_DECOR_POSITION_BOTTOM_LEFT_CORNER;
    }
    else if (surface == client->decor.corner_bottom_right_surface)
    {
        client->cursor_decor_position &= ~CURSOR_DECOR_POSITION_BOTTOM_RIGHT_CORNER;
    }
    else if (surface == client->decor.border_top_surface)
    {
        client->cursor_decor_position &= ~CURSOR_DECOR_POSITION_TOP_BORDER;
    }
    else if (surface == client->decor.border_bottom_surface)
    {
        client->cursor_decor_position &= ~CURSOR_DECOR_POSITION_BOTTOM_BORDER;
    }
    else if (surface == client->decor.border_left_surface)
    {
        client->cursor_decor_position &= ~CURSOR_DECOR_POSITION_LEFT_BORDER;
    }
    else if (surface == client->decor.border_right_surface)
    {
        client->cursor_decor_position &= ~CURSOR_DECOR_POSITION_RIGHT_BORDER;
    }
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

    if (client->cursor_decor_position & CURSOR_DECOR_POSITION_TITLEBAR)
    {
        xdg_toplevel_move(client->xdg_toplevel, client->seat, serial);
    }
    if (client->cursor_decor_position & CURSOR_DECOR_POSITION_CLOSE_BUTTON)
    {
        client->should_close = true;
    }
    else if (client->cursor_decor_position & CURSOR_DECOR_POSITION_TOP_LEFT_CORNER)
    {
        xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT);
    }
    else if (client->cursor_decor_position & CURSOR_DECOR_POSITION_TOP_RIGHT_CORNER)
    {
        xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT);
    }
    else if (client->cursor_decor_position & CURSOR_DECOR_POSITION_BOTTOM_LEFT_CORNER)
    {
        xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT);
    }
    else if (client->cursor_decor_position & CURSOR_DECOR_POSITION_BOTTOM_RIGHT_CORNER)
    {
        xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
    }
    else if (client->cursor_decor_position & CURSOR_DECOR_POSITION_TOP_BORDER)
    {
        xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_TOP);
    }
    else if (client->cursor_decor_position & CURSOR_DECOR_POSITION_BOTTOM_BORDER)
    {
        xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM);
    }
    else if (client->cursor_decor_position & CURSOR_DECOR_POSITION_LEFT_BORDER)
    {
        xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_LEFT);
    }
    else if (client->cursor_decor_position & CURSOR_DECOR_POSITION_RIGHT_BORDER)
    {
        xdg_toplevel_resize(client->xdg_toplevel, client->seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_RIGHT);
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
    printf("info (wayland): Registred interface `%s-%d`.\n", interface, version);

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
    else if (strcmp(interface, wl_subcompositor_interface.name) == 0)
    {
        client->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
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

    client.decor.titlebar_surface = wl_compositor_create_surface(client.compositor);
    client.decor.titlebar_subsurface = wl_subcompositor_get_subsurface(client.subcompositor, client.decor.titlebar_surface, client.surface);
    client.decor.close_button_surface = wl_compositor_create_surface(client.compositor);
    client.decor.close_button_subsurface = wl_subcompositor_get_subsurface(client.subcompositor, client.decor.close_button_surface, client.surface);
    client.decor.border_top_surface = wl_compositor_create_surface(client.compositor);
    client.decor.border_top_subsurface = wl_subcompositor_get_subsurface(client.subcompositor, client.decor.border_top_surface, client.surface);
    client.decor.border_bottom_surface = wl_compositor_create_surface(client.compositor);
    client.decor.border_bottom_subsurface = wl_subcompositor_get_subsurface(client.subcompositor, client.decor.border_bottom_surface, client.surface);
    client.decor.border_left_surface = wl_compositor_create_surface(client.compositor);
    client.decor.border_left_subsurface = wl_subcompositor_get_subsurface(client.subcompositor, client.decor.border_left_surface, client.surface);
    client.decor.border_right_surface = wl_compositor_create_surface(client.compositor);
    client.decor.border_right_subsurface = wl_subcompositor_get_subsurface(client.subcompositor, client.decor.border_right_surface, client.surface);
    client.decor.corner_top_left_surface = wl_compositor_create_surface(client.compositor);
    client.decor.corner_top_left_subsurface = wl_subcompositor_get_subsurface(client.subcompositor, client.decor.corner_top_left_surface, client.surface);
    client.decor.corner_top_right_surface = wl_compositor_create_surface(client.compositor);
    client.decor.corner_top_right_subsurface = wl_subcompositor_get_subsurface(client.subcompositor, client.decor.corner_top_right_surface, client.surface);
    client.decor.corner_bottom_left_surface = wl_compositor_create_surface(client.compositor);
    client.decor.corner_bottom_left_subsurface = wl_subcompositor_get_subsurface(client.subcompositor, client.decor.corner_bottom_left_surface, client.surface);
    client.decor.corner_bottom_right_surface = wl_compositor_create_surface(client.compositor);
    client.decor.corner_bottom_right_subsurface = wl_subcompositor_get_subsurface(client.subcompositor, client.decor.corner_bottom_right_surface, client.surface);

    struct wl_cursor_theme *cursor_theme = wl_cursor_theme_load(NULL, 24, client.shm);

    struct wl_cursor *cursor = wl_cursor_theme_get_cursor(cursor_theme, "left_ptr");
    client.cursor_images[CURSOR_VARIANT_LEFT_PTR] = cursor->images[0];
    client.cursor_buffers[CURSOR_VARIANT_LEFT_PTR] = wl_cursor_image_get_buffer(client.cursor_images[CURSOR_VARIANT_LEFT_PTR]);

    cursor = wl_cursor_theme_get_cursor(cursor_theme, "pointer");
    client.cursor_images[CURSOR_VARIANT_POINTER] = cursor->images[0];
    client.cursor_buffers[CURSOR_VARIANT_POINTER] = wl_cursor_image_get_buffer(client.cursor_images[CURSOR_VARIANT_POINTER]);

    cursor = wl_cursor_theme_get_cursor(cursor_theme, "n-resize");
    client.cursor_images[CURSOR_VARIANT_N_RESIZE] = cursor->images[0];
    client.cursor_buffers[CURSOR_VARIANT_N_RESIZE] = wl_cursor_image_get_buffer(client.cursor_images[CURSOR_VARIANT_N_RESIZE]);

    cursor = wl_cursor_theme_get_cursor(cursor_theme, "s-resize");
    client.cursor_images[CURSOR_VARIANT_S_RESIZE] = cursor->images[0];
    client.cursor_buffers[CURSOR_VARIANT_S_RESIZE] = wl_cursor_image_get_buffer(client.cursor_images[CURSOR_VARIANT_S_RESIZE]);

    cursor = wl_cursor_theme_get_cursor(cursor_theme, "w-resize");
    client.cursor_images[CURSOR_VARIANT_W_RESIZE] = cursor->images[0];
    client.cursor_buffers[CURSOR_VARIANT_W_RESIZE] = wl_cursor_image_get_buffer(client.cursor_images[CURSOR_VARIANT_W_RESIZE]);

    cursor = wl_cursor_theme_get_cursor(cursor_theme, "e-resize");
    client.cursor_images[CURSOR_VARIANT_E_RESIZE] = cursor->images[0];
    client.cursor_buffers[CURSOR_VARIANT_E_RESIZE] = wl_cursor_image_get_buffer(client.cursor_images[CURSOR_VARIANT_E_RESIZE]);

    cursor = wl_cursor_theme_get_cursor(cursor_theme, "nw-resize");
    client.cursor_images[CURSOR_VARIANT_NW_RESIZE] = cursor->images[0];
    client.cursor_buffers[CURSOR_VARIANT_NW_RESIZE] = wl_cursor_image_get_buffer(client.cursor_images[CURSOR_VARIANT_NW_RESIZE]);

    cursor = wl_cursor_theme_get_cursor(cursor_theme, "ne-resize");
    client.cursor_images[CURSOR_VARIANT_NE_RESIZE] = cursor->images[0];
    client.cursor_buffers[CURSOR_VARIANT_NE_RESIZE] = wl_cursor_image_get_buffer(client.cursor_images[CURSOR_VARIANT_NE_RESIZE]);

    cursor = wl_cursor_theme_get_cursor(cursor_theme, "sw-resize");
    client.cursor_images[CURSOR_VARIANT_SW_RESIZE] = cursor->images[0];
    client.cursor_buffers[CURSOR_VARIANT_SW_RESIZE] = wl_cursor_image_get_buffer(client.cursor_images[CURSOR_VARIANT_SW_RESIZE]);

    cursor = wl_cursor_theme_get_cursor(cursor_theme, "se-resize");
    client.cursor_images[CURSOR_VARIANT_SE_RESIZE] = cursor->images[0];
    client.cursor_buffers[CURSOR_VARIANT_SE_RESIZE] = wl_cursor_image_get_buffer(client.cursor_images[CURSOR_VARIANT_SE_RESIZE]);

    client.cursor_surface = wl_compositor_create_surface(client.compositor);

    printf("Use the Escape key to close the window.\n");

    while (wl_display_dispatch(client.display) != -1 && !client.should_close)
    {

    }
}
