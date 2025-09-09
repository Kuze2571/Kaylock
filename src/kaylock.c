#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <linux/input.h>
#include <xkbcommon/xkbcommon.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <time.h>
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <math.h>

#include "xdg-shell-client-protocol.h"

// Recovery password, can be deactivated or modified at your convenience
#define BYPASS_PASSWORD "debug123"
#define MAX_PASSWORD_LEN 256

struct wayland_locker {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    struct xdg_wm_base *xdg_wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_shm *shm;

    // xkbcommon keyboard management
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    int width, height;
    int running;
    char input_buffer[MAX_PASSWORD_LEN];
    int input_position;
    char *username;
    int debug_mode;

    // display settings
    int blur_radius;
    int input_changed;  // Unused flag in final version
    time_t last_keypress;

    struct wl_buffer *buffer;
    void *shm_data;
    int shm_size;
};

// PAM implementation
struct pam_response_data {
    const char *password;
    int error_code;
};

// Declared functions
static void draw_background(struct wayland_locker *locker);
static void update_display(struct wayland_locker *locker);

// PAM related functions
static int pam_conversation(int num_msg, const struct pam_message **msg,
                           struct pam_response **resp, void *appdata_ptr) {
    struct pam_response_data *pam_data = (struct pam_response_data *)appdata_ptr;
    struct pam_response *response;

    if (num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG) {
        return PAM_CONV_ERR;
    }

    response = calloc(num_msg, sizeof(struct pam_response));
    if (response == NULL) {
        return PAM_BUF_ERR;
    }

    for (int i = 0; i < num_msg; i++) {
        switch (msg[i]->msg_style) {
            case PAM_PROMPT_ECHO_OFF:
                response[i].resp = strdup(pam_data->password);
                response[i].resp_retcode = 0;
                break;
            case PAM_ERROR_MSG:
                fprintf(stderr, "PAM error: %s\n", msg[i]->msg);
                break;
            case PAM_TEXT_INFO:
                printf("PAM info: %s\n", msg[i]->msg);
                break;
            default:
                free(response);
                return PAM_CONV_ERR;
        }
    }

    *resp = response;
    return PAM_SUCCESS;
}

// Window management +add
static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

// Listener function +add
static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping,
};



// PAM authentication
static int authenticate_user(const char *username, const char *password, int debug_mode) {
    // password displayed in the console if debug mode is on
    if (debug_mode) {
        printf("Authentication attempt for '%s'\n", username);
        printf("Password: [%s]\n", password);
        printf("Password length: %ld characters\n", strlen(password));
    }

    // Recovery password function
    if (strcmp(password, BYPASS_PASSWORD) == 0 && debug_mode) {
        printf("Recovery password accepted. (BYPASS_PASSWORD)\n");
        return 1;
    }

    pam_handle_t *pamh = NULL;
    struct pam_conv conv;
    struct pam_response_data pam_data;
    int retval;

    pam_data.password = password;
    pam_data.error_code = 0;

    conv.conv = pam_conversation;
    conv.appdata_ptr = &pam_data;

    retval = pam_start("kaylock", username, &conv, &pamh);
    if (retval != PAM_SUCCESS) {
        fprintf(stderr, "PAM start failed: %s\n", pam_strerror(pamh, retval));
        return 0;
    }

    retval = pam_authenticate(pamh, 0);
    if (retval != PAM_SUCCESS) {
        fprintf(stderr, "Authentication failed: %s\n", pam_strerror(pamh, retval));
        pam_end(pamh, retval);
        return 0;
    }

    retval = pam_acct_mgmt(pamh, 0);
    if (retval != PAM_SUCCESS) {
        fprintf(stderr, "Account validation failed: %s\n", pam_strerror(pamh, retval));
        pam_end(pamh, retval);
        return 0;
    }

    pam_end(pamh, PAM_SUCCESS);
    return 1;
}

static void handle_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface, uint32_t version) {
    struct wayland_locker *locker = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        locker->compositor = wl_registry_bind(registry, name,
                                            &wl_compositor_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        locker->xdg_wm_base = wl_registry_bind(registry, name,
                                             &xdg_wm_base_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        locker->seat = wl_registry_bind(registry, name,
                                      &wl_seat_interface, 1);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        locker->shm = wl_registry_bind(registry, name,
                                      &wl_shm_interface, 1);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry,
                                uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                 uint32_t serial) {
    struct wayland_locker *locker = data;
    xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                                  int32_t width, int32_t height,
                                  struct wl_array *states) {
    struct wayland_locker *locker = data;
    if (width > 0 && height > 0) {
        locker->width = width;
        locker->height = height;
    }
}

// Alt+F4 management +add
static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    struct wayland_locker *locker = data;

    if (locker->debug_mode) {
        printf("Closing attempt with Alt+F4 (blocked)\n");
    }
    wl_surface_commit(locker->surface);
    // Commented // locker->running = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

// keymap management
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                          uint32_t format, int32_t fd, uint32_t size) {
    struct wayland_locker *locker = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        fprintf(stderr, "Keymap formats not supported\n");
        return;
    }

    // Keymap mapping
    char *keymap_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (keymap_str == MAP_FAILED) {
        close(fd);
        fprintf(stderr, "Keymap mapping failed\n");
        return;
    }

    // XKB context creation
    if (locker->xkb_context == NULL) {
        locker->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (locker->xkb_context == NULL) {
            munmap(keymap_str, size);
            close(fd);
            fprintf(stderr, "XKB context creation failed\n");
            return;
        }
    }

    // Keymap loading
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(locker->xkb_context,
                                                        keymap_str,
                                                        XKB_KEYMAP_FORMAT_TEXT_V1,
                                                        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(keymap_str, size);
    close(fd);

    if (keymap == NULL) {
        fprintf(stderr, "Keymap compilation failed\n");
        return;
    }

    // XKB state creation
    struct xkb_state *state = xkb_state_new(keymap);
    if (state == NULL) {
        xkb_keymap_unref(keymap);
        fprintf(stderr, "XKB state creation failed\n");
        return;
    }

    // Cleaning XKB state
    if (locker->xkb_state)
        xkb_state_unref(locker->xkb_state);
    if (locker->xkb_keymap)
        xkb_keymap_unref(locker->xkb_keymap);

    // Loading new objects
    locker->xkb_keymap = keymap;
    locker->xkb_state = state;

    if (locker->debug_mode) {
        printf("XKB keymap successfully loaded \n");
    }
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                          uint32_t serial, struct wl_surface *surface,
                          struct wl_array *keys) {
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                          uint32_t serial, struct wl_surface *surface) {
}

// Main display update function
static void update_display(struct wayland_locker *locker) {
    // Elements redraw
    draw_background(locker);
    wl_surface_attach(locker->surface, locker->buffer, 0, 0);
    wl_surface_damage(locker->surface, 0, 0, locker->width, locker->height);
    wl_surface_commit(locker->surface);
}

// XKB keys management
static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                        uint32_t serial, uint32_t time, uint32_t key,
                        uint32_t state) {
    struct wayland_locker *locker = data;

    // xkbcommon adjustment variable (minus 8)
    uint32_t xkb_key = key + 8;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        locker->last_keypress++;

        // Debug mode key displayed
        if (locker->debug_mode) {
            printf("Pressed key: %u (xkb: %u)", key, xkb_key);
        }

        if (locker->xkb_state) {
            xkb_keysym_t keysym = xkb_state_key_get_one_sym(locker->xkb_state, xkb_key);

            if (locker->debug_mode) {
                char keysym_name[64];
                xkb_keysym_get_name(keysym, keysym_name, sizeof(keysym_name));
                printf(" - Symbole: %s", keysym_name);
            }

            // Alt+F4 block
            if (keysym == XKB_KEY_F4 &&
                xkb_state_mod_name_is_active(locker->xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE)) {
                if (locker->debug_mode) {
                    printf(" (ALT+F4 blocked)\n");
                }
                return;
            }

            // Special keys
            if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter) {
                // debug mode display
                if (locker->debug_mode) {
                    printf("\n=== PASSWORD CHECK ===\n");
                    printf("Input buffer: [%s]\n", locker->input_buffer);
                    printf("Length: %d characters\n", locker->input_position);
                    printf("===============================\n");
                }

                // Password check
                locker->input_buffer[locker->input_position] = '\0';

                if (authenticate_user(locker->username, locker->input_buffer, locker->debug_mode)) {
                    printf("Authentication success, unlocking\n");
                    locker->running = 0;  
                } else {
                    printf("Authentication failed\n");
                    // Buffer Reset
                    memset(locker->input_buffer, 0, sizeof(locker->input_buffer));
                    locker->input_position = 0;
                    locker->input_changed = 1;  
                    update_display(locker);  // Display update
                }
            }
            else if (keysym == XKB_KEY_BackSpace) {
                if (locker->input_position > 0) {
                    locker->input_position--;
                    locker->input_buffer[locker->input_position] = '\0';
                    locker->input_changed = 1;
                    //update_display(locker);  // Deactivated function
                    if (locker->debug_mode) printf(" (BACKSPACE)");
                }
            }
            else if (keysym == XKB_KEY_Escape) {
                if (locker->debug_mode) printf(" (ESCAPE)");
                // Do nothing with escape
            }
            else {
                // Get UTF-8 key
                char utf8_buffer[8] = {0};
                int len = xkb_keysym_to_utf8(keysym, utf8_buffer, sizeof(utf8_buffer));

                if (len > 1 && locker->input_position < MAX_PASSWORD_LEN - len) {  // len includes null terminator
                    strcpy(locker->input_buffer + locker->input_position, utf8_buffer);
                    locker->input_position += len - 1;  // -1 in null terminator
                    locker->input_changed = 1;
                    //update_display(locker);  // Deactivated function
                    if (locker->debug_mode) printf(" -> '%s'", utf8_buffer);
                }
            }
        }

        if (locker->debug_mode) printf("\n");
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                              uint32_t serial, uint32_t mods_depressed,
                              uint32_t mods_latched, uint32_t mods_locked,
                              uint32_t group) {
    struct wayland_locker *locker = data;

    // Update modificators in XKB
    if (locker->xkb_state) {
        xkb_state_update_mask(locker->xkb_state,
                             mods_depressed, mods_latched, mods_locked,
                             0, 0, group);

        if (locker->debug_mode) {
            printf("Modificators: depressed=%u, latched=%u, locked=%u, group=%u\n",
                  mods_depressed, mods_latched, mods_locked, group);
        }
    }
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
};

static void pointer_enter(void *data, struct wl_pointer *pointer,
                         uint32_t serial, struct wl_surface *surface,
                         wl_fixed_t sx, wl_fixed_t sy) {
    // ignoring mouse
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                         uint32_t serial, struct wl_surface *surface) {
    // ignoring mouse
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                          uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    // ignoring mouse
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                          uint32_t serial, uint32_t time, uint32_t button,
                          uint32_t state) {
    // ignoring mouse
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                        uint32_t time, uint32_t axis, wl_fixed_t value) {
    // ignoring mouse
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                             uint32_t capabilities) {
    struct wayland_locker *locker = data;

    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        locker->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(locker->keyboard, &keyboard_listener, locker);
    }

    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        locker->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(locker->pointer, &pointer_listener, locker);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

// Shared memory file creation
static int create_shm_file(size_t size) {
    int fd = memfd_create("kaylock-buffer", 0);
    if (fd < 0) {
        return -1;
    }

    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// Buffer creation
static struct wl_buffer *create_buffer(struct wayland_locker *locker) {
    int stride = locker->width * 4; // 4 bytes per pixel (ARGB32)
    locker->shm_size = stride * locker->height;

    int fd = create_shm_file(locker->shm_size);
    if (fd < 0) {
        fprintf(stderr, "Shared memory file creation error\n");
        return NULL;
    }

    locker->shm_data = mmap(NULL, locker->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (locker->shm_data == MAP_FAILED) {
        fprintf(stderr, "Memory mapping error\n");
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(locker->shm, fd, locker->shm_size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
                                                       locker->width, locker->height,
                                                       stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    return buffer;
}

// Blur function
static void apply_blur(uint32_t *pixels, int width, int height, int radius) {
    double *temp = malloc(width * height * sizeof(double) * 4);
    double *kernel = malloc((radius*2+1) * sizeof(double));

    if (!temp || !kernel) {
        fprintf(stderr, "Erreur d'allocation mémoire pour le flou\n");
        if (temp) free(temp);
        if (kernel) free(kernel);
        return;
    }

    // New blur creation
    double sigma = radius / 3.0;
    double sum = 0.0;
    for (int i = -radius; i <= radius; i++) {
        kernel[i+radius] = exp(-(i*i) / (2*sigma*sigma));
        sum += kernel[i+radius];
    }

    // Kernel normalisation
    for (int i = 0; i <= 2*radius; i++) {
        kernel[i] /= sum;
    }

    // copy RGBA components in temp memory
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t pixel = pixels[y*width + x];
            temp[(y*width + x)*4 + 0] = ((pixel >> 16) & 0xFF);  // R
            temp[(y*width + x)*4 + 1] = ((pixel >> 8) & 0xFF);   // G
            temp[(y*width + x)*4 + 2] = (pixel & 0xFF);          // B
            temp[(y*width + x)*4 + 3] = ((pixel >> 24) & 0xFF);  // A
        }
    }

    // Horizontal blur
    double *hblur = malloc(width * height * sizeof(double) * 4);
    if (!hblur) {
        fprintf(stderr, "Horizontal blur memory allocation error\n");
        free(temp);
        free(kernel);
        return;
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            double r = 0, g = 0, b = 0, a = 0;
            double weight = 0;

            for (int i = -radius; i <= radius; i++) {
                int sx = x + i;
                if (sx >= 0 && sx < width) {
                    double k = kernel[i+radius];
                    r += temp[(y*width + sx)*4 + 0] * k;
                    g += temp[(y*width + sx)*4 + 1] * k;
                    b += temp[(y*width + sx)*4 + 2] * k;
                    a += temp[(y*width + sx)*4 + 3] * k;
                    weight += k;
                }
            }

            // Normalisation
            if (weight > 0) {
                hblur[(y*width + x)*4 + 0] = r / weight;
                hblur[(y*width + x)*4 + 1] = g / weight;
                hblur[(y*width + x)*4 + 2] = b / weight;
                hblur[(y*width + x)*4 + 3] = a / weight;
            }
        }
    }

    // Vertical blur
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            double r = 0, g = 0, b = 0, a = 0;
            double weight = 0;

            for (int i = -radius; i <= radius; i++) {
                int sy = y + i;
                if (sy >= 0 && sy < height) {
                    double k = kernel[i+radius];
                    r += hblur[(sy*width + x)*4 + 0] * k;
                    g += hblur[(sy*width + x)*4 + 1] * k;
                    b += hblur[(sy*width + x)*4 + 2] * k;
                    a += hblur[(sy*width + x)*4 + 3] * k;
                    weight += k;
                }
            }

            // Result storage
            if (weight > 0) {
                int ir = (int)(r / weight);
                int ig = (int)(g / weight);
                int ib = (int)(b / weight);
                int ia = (int)(a / weight);

                // Values limited between 0 & 255
                ir = ir < 0 ? 0 : (ir > 255 ? 255 : ir);
                ig = ig < 0 ? 0 : (ig > 255 ? 255 : ig);
                ib = ib < 0 ? 0 : (ib > 255 ? 255 : ib);
                ia = ia < 0 ? 0 : (ia > 255 ? 255 : ia);

                pixels[y*width + x] = (ia << 24) | (ir << 16) | (ig << 8) | ib;
            }
        }
    }

    // Free memory
    free(temp);
    free(kernel);
    free(hblur);
}

// Background draw with Cairo & Pango
static void draw_background(struct wayland_locker *locker) {
    // Optional: screenshot and blur

    // Simpler method : drawing a half-transparent blur
    uint32_t *pixels = locker->shm_data;

    // Blue-black fade
    for (int y = 0; y < locker->height; y++) {
        for (int x = 0; x < locker->width; x++) {
            float gradient = 1.0f - (float)y / locker->height;

            uint8_t r = (uint8_t)(20 * gradient);
            uint8_t g = (uint8_t)(20 * gradient);
            uint8_t b = (uint8_t)(20 * gradient);
            uint8_t a = 220;  // Plus opaque

            pixels[y * locker->width + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    apply_blur(pixels, locker->width, locker->height, locker->blur_radius);

    // Cairo surface creation
    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        (unsigned char*)pixels,
        CAIRO_FORMAT_ARGB32,
        locker->width,
        locker->height,
        locker->width * 4
    );

    cairo_t *cr = cairo_create(surface);

    // Finally unused function
    if (locker->input_position > 0) {
        // Configure Pango for the text
        PangoLayout *layout = pango_cairo_create_layout(cr);
        PangoFontDescription *font_desc = pango_font_description_from_string("Sans Bold 24");
        pango_layout_set_font_description(layout, font_desc);
        pango_font_description_free(font_desc);
        
        // empty strings instead of *
        char password_display[1] = {0};
        
        pango_layout_set_text(layout, password_display, -1);

        int text_width, text_height;
        pango_layout_get_size(layout, &text_width, &text_height);
        text_width /= PANGO_SCALE;
        text_height /= PANGO_SCALE;

        // Center text
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);  // Blanc semi-transparent
        cairo_move_to(cr,
                     (locker->width - text_width) / 2,
                     (locker->height - text_height) / 2);

        // Text draw
        pango_cairo_show_layout(cr, layout);

        g_object_unref(layout);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

// Help function
static void show_help(const char *prog_name) {
    printf("Usage: %s [options]\n\n", prog_name);
    printf("Options:\n");
    printf("  -h, --help       Show help\n");
    printf("  -d, --debug      Activate debug mode (verbose)\n");
    printf("  -b, --blur N     Define blur level (0-20, default: 8)\n");
    printf("\nIn debug mode, a recovery password is activated : '%s'\n", BYPASS_PASSWORD);
}

int main(int argc, char **argv) {
    struct wayland_locker locker = {0};
    locker.running = 1;
    locker.width = 1920;  // defaullt values
    locker.height = 1080;
    locker.debug_mode = 0; // Deactivated by default
    locker.blur_radius = 8; // Default blur level

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            show_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            locker.debug_mode = 1;
            printf("Mode debug activé\n");
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--blur") == 0) {
            if (i + 1 < argc) {
                locker.blur_radius = atoi(argv[i+1]);
                i++; // Jump to the next args
                if (locker.blur_radius < 0) locker.blur_radius = 0;
                if (locker.blur_radius > 20) locker.blur_radius = 20;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            show_help(argv[0]);
            return 1;
        }
    }

    // Get username
    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    if (!pw) {
        fprintf(stderr, "Impossible to get current username\n");
        return 1;
    }
    locker.username = strdup(pw->pw_name);
    printf("Username detected: %s\n", locker.username);

    // initiate Wayland connection
    locker.display = wl_display_connect(NULL);
    if (!locker.display) {
        fprintf(stderr, "Impossible to join Wayland server\n");
        free(locker.username);
        return 1;
    }

    // Initiate XKB context
    locker.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!locker.xkb_context) {
        fprintf(stderr, "Impossible to create XKB context\n");
        wl_display_disconnect(locker.display);
        free(locker.username);
        return 1;
    }

    // get registers
    locker.registry = wl_display_get_registry(locker.display);
    wl_registry_add_listener(locker.registry, &registry_listener, &locker);
    wl_display_roundtrip(locker.display);

    //xdg_wm_base_add_listener(locker.xdg_wm_base, &wm_base_listener, &locker); // +add

    // Check on all necessary objects at this point
    if (!locker.compositor || !locker.xdg_wm_base || !locker.seat || !locker.shm) {
        fprintf(stderr, "Impossible to get necessary Wayland interfaces\n");
        if (locker.xkb_context) xkb_context_unref(locker.xkb_context);
        wl_display_disconnect(locker.display);
        free(locker.username);
        return 1;
    }

    xdg_wm_base_add_listener(locker.xdg_wm_base, &wm_base_listener, &locker); // +add    
    // Get keyboard and mouse
    wl_seat_add_listener(locker.seat, &seat_listener, &locker);
    wl_display_roundtrip(locker.display);

    // Surface creation
    locker.surface = wl_compositor_create_surface(locker.compositor);

    // Configure XDG surface
    locker.xdg_surface = xdg_wm_base_get_xdg_surface(locker.xdg_wm_base, locker.surface);
    xdg_surface_add_listener(locker.xdg_surface, &xdg_surface_listener, &locker);

    // Toplevel configuration
    locker.xdg_toplevel = xdg_surface_get_toplevel(locker.xdg_surface);
    xdg_toplevel_add_listener(locker.xdg_toplevel, &xdg_toplevel_listener, &locker);
    xdg_toplevel_set_title(locker.xdg_toplevel, "Wayland Locker");
    xdg_toplevel_set_fullscreen(locker.xdg_toplevel, NULL);

    wl_surface_commit(locker.surface);

    wl_display_roundtrip(locker.display);

    // Create and configure the buffer
    locker.buffer = create_buffer(&locker);
    if (!locker.buffer) {
        fprintf(stderr, "Impossible to create buffer\n");
        if (locker.xkb_context) xkb_context_unref(locker.xkb_context);
        wl_display_disconnect(locker.display);
        free(locker.username);
        return 1;
    }

    // Draw initial backgroud
    draw_background(&locker);

    // Attach buffer to the surface
    wl_surface_attach(locker.surface, locker.buffer, 0, 0);
    wl_surface_damage(locker.surface, 0, 0, locker.width, locker.height);
    wl_surface_commit(locker.surface);

    printf("Screen locked. Type your password and press ENTER to unlock.\n");
    if (locker.debug_mode) {
        printf("Debug mode activated. You can use '%s' as a recovery password.\n", BYPASS_PASSWORD);
    }

    while (locker.running) {
        wl_display_dispatch(locker.display);
    }

    // Cleaning
    if (locker.buffer)
        wl_buffer_destroy(locker.buffer);
    if (locker.shm_data)
        munmap(locker.shm_data, locker.shm_size);

    if (locker.xkb_state)
        xkb_state_unref(locker.xkb_state);
    if (locker.xkb_keymap)
        xkb_keymap_unref(locker.xkb_keymap);
    if (locker.xkb_context)
        xkb_context_unref(locker.xkb_context);

    xdg_toplevel_destroy(locker.xdg_toplevel);
    xdg_surface_destroy(locker.xdg_surface);
    wl_surface_destroy(locker.surface);
    if (locker.pointer)
        wl_pointer_destroy(locker.pointer);
    if (locker.keyboard)
        wl_keyboard_destroy(locker.keyboard);
    wl_seat_destroy(locker.seat);
    xdg_wm_base_destroy(locker.xdg_wm_base);
    wl_compositor_destroy(locker.compositor);
    wl_registry_destroy(locker.registry);
    wl_display_disconnect(locker.display);

    free(locker.username);

    return 0;
}
