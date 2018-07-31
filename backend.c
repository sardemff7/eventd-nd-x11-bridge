/*
 * eventd - Small daemon to act on remote or local events
 *
 * Copyright © 2011-2018 Quentin "Sardem FF7" Glidic
 *
 * This file is part of eventd.
 *
 * eventd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * eventd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with eventd. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <compositor.h>
#include <cairo.h>
#include <cairo-xcb.h>
#include "libgwater-xcb.h"
#include <xcb/shm.h>
#include <xcb/randr.h>
#include <xcb/xkb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xfixes.h>
#include <xkbcommon/xkbcommon-x11.h>

#include "backend.h"

typedef struct {
    struct weston_backend base;
    struct weston_compositor *compositor;
    ENXBBackendConfig config;

    GWaterXcbSource *source;
    xcb_connection_t *xcb_connection;
    gint display;
    gint screen_number;
    xcb_screen_t *screen;
    guint8 depth;
    xcb_visualtype_t *visual;
    xcb_colormap_t map;
    gboolean randr;
    gboolean xkb;
    gboolean compositing;
    gboolean custom_map;
    gboolean xfixes;
    xcb_ewmh_connection_t ewmh;
    gint randr_event_base;
    guint8 xkb_event_base;
    gint xfixes_event_base;
    gint32 xkb_device_id;
    struct xkb_context *xkb_context;

    GHashTable *heads;
    struct weston_seat core_seat;
    struct weston_output *output;
    GHashTable *views;
} ENXBBackend;

typedef struct {
    struct weston_output base;
    gint finish_frame_timer;
} ENXBOutput;

typedef struct {
    struct weston_head base;
    struct weston_mode mode;
    ENXBOutput output;
} ENXBHead;

typedef struct {
    struct wl_listener destroy_listener;
    ENXBBackend *backend;
    struct weston_surface *surface;
    struct weston_buffer_reference buffer_ref;
    cairo_surface_t *cairo_surface;
    struct weston_size size;
} ENXBSurface;

typedef struct {
    struct wl_listener destroy_listener;
    ENXBBackend *backend;
    struct weston_view *view;
    ENXBSurface *surface;
    xcb_window_t window;
    cairo_surface_t *cairo_surface;
    gboolean mapped;
} ENXBView;

static void
_enxb_surface_destroy_notify(struct wl_listener *listener, void *data)
{
    ENXBSurface *self = wl_container_of(listener, self, destroy_listener);

    cairo_surface_destroy(self->cairo_surface);
    weston_buffer_reference(&self->buffer_ref, NULL);

    g_free(self);
}

static ENXBSurface *
_enxb_surface_new(ENXBBackend *backend, struct weston_surface *surface)
{
    ENXBSurface *self;
    self = g_new0(ENXBSurface, 1);
    self->backend = backend;
    self->surface = surface;

    self->destroy_listener.notify = _enxb_surface_destroy_notify;
    wl_signal_add(&self->surface->destroy_signal, &self->destroy_listener);

    return self;
}

static ENXBSurface *
_enxb_surface_from_weston_surface(ENXBBackend *backend, struct weston_surface *surface)
{
    struct wl_listener *listener;
    ENXBSurface *self;

    listener = wl_signal_get(&surface->destroy_signal, _enxb_surface_destroy_notify);
    if ( listener != NULL )
        return wl_container_of(listener, self, destroy_listener);

    return _enxb_surface_new(backend, surface);
}

static int
_enxb_renderer_read_pixels(struct weston_output *output, pixman_format_code_t format, void *pixels, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    return -1;
}

static void
_enxb_renderer_repaint_output(struct weston_output *output, pixman_region32_t *output_damage)
{
}

static void
_enxb_renderer_flush_damage(struct weston_surface *surface)
{
}

static gboolean
_enxb_surface_attach_shm(ENXBSurface *surface, struct wl_shm_buffer *buffer)
{
    cairo_format_t format;
    switch ( wl_shm_buffer_get_format(buffer) )
    {
    case WL_SHM_FORMAT_XRGB8888:
        format = CAIRO_FORMAT_RGB24;
    break;
    case WL_SHM_FORMAT_ARGB8888:
        format = CAIRO_FORMAT_ARGB32;
    break;
    case WL_SHM_FORMAT_RGB565:
        format = CAIRO_FORMAT_RGB16_565;
    break;
    case WL_SHM_FORMAT_RGBX1010102:
        format = CAIRO_FORMAT_RGB30;
    break;
    default:
        g_warning("Unsupported SHM buffer format");
        return FALSE;
    }


    gint stride = wl_shm_buffer_get_stride(buffer);
    surface->size.width = wl_shm_buffer_get_width(buffer);
    surface->size.height = wl_shm_buffer_get_height(buffer);

    surface->cairo_surface = cairo_image_surface_create_for_data(wl_shm_buffer_get_data(buffer), format, surface->size.width, surface->size.height, stride);
    if ( cairo_surface_status(surface->cairo_surface) != CAIRO_STATUS_SUCCESS )
        return FALSE;

    weston_compositor_schedule_repaint(surface->backend->compositor);
    return TRUE;
}

static void
_enxb_renderer_attach(struct weston_surface *wsurface, struct weston_buffer *buffer)
{
    ENXBBackend *backend = wl_container_of(wsurface->compositor->backend, backend, base);
    ENXBSurface *surface = _enxb_surface_from_weston_surface(backend, wsurface);
    struct wl_shm_buffer *shm_buffer;

    weston_buffer_reference(&surface->buffer_ref, buffer);

    shm_buffer = wl_shm_buffer_get(buffer->resource);
    if ( shm_buffer != NULL )
    {
        if ( _enxb_surface_attach_shm(surface, shm_buffer) )
            return;
    }
    weston_buffer_reference(&surface->buffer_ref, NULL);
}

static void
_enxb_renderer_surface_set_color(struct weston_surface *surface, float red, float green, float blue, float alpha)
{
}

static void
_enxb_renderer_destroy(struct weston_compositor *compositor)
{
}


/** See weston_surface_get_content_size() */
static void
_enxb_renderer_surface_get_content_size(struct weston_surface *surface, int *width, int *height)
{
    *width = 0;
    *height = 0;
}

/** See weston_surface_copy_content() */
static int
_enxb_renderer_surface_copy_content(struct weston_surface *surface, void *target, size_t size, int src_x, int src_y, int width, int height)
{
    return -1;
}

/** See weston_compositor_import_dmabuf() */
static bool
_enxb_renderer_import_dmabuf(struct weston_compositor *compositor, struct linux_dmabuf_buffer *buffer)
{
    return false;
}

/** On error sets num_formats to zero */
static void
_enxb_renderer_query_dmabuf_formats(struct weston_compositor *compositor, int **formats, int *num_formats)
{
    *formats = NULL;
    *num_formats = 0;
}

/** On error sets num_modifiers to zero */
static void
_enxb_renderer_query_dmabuf_modifiers(struct weston_compositor *compositor, int format, uint64_t **modifiers, int *num_modifiers)
{
    *modifiers = NULL;
    *num_modifiers = 0;
}

static struct weston_renderer _enxb_renderer = {
    .read_pixels = _enxb_renderer_read_pixels,
    .repaint_output = _enxb_renderer_repaint_output,
    .flush_damage = _enxb_renderer_flush_damage,
    .attach = _enxb_renderer_attach,
    .surface_set_color = _enxb_renderer_surface_set_color,
    .destroy = _enxb_renderer_destroy,
    .surface_get_content_size = _enxb_renderer_surface_get_content_size,
    .surface_copy_content = _enxb_renderer_surface_copy_content,
    .import_dmabuf = _enxb_renderer_import_dmabuf,
    .query_dmabuf_formats = _enxb_renderer_query_dmabuf_formats,
    .query_dmabuf_modifiers = _enxb_renderer_query_dmabuf_modifiers,

};


static void
_enxb_view_destroy_notify(struct wl_listener *listener, void *data)
{
    ENXBView *self = wl_container_of(listener, self, destroy_listener);

    cairo_surface_flush(self->cairo_surface);
    cairo_surface_destroy(self->cairo_surface);
    xcb_destroy_window(self->backend->xcb_connection, self->window);

    g_hash_table_remove(self->backend->views, GINT_TO_POINTER(self->window));

    g_free(self);
}

static ENXBView *
_enxb_view_new(ENXBBackend *backend, struct weston_view *view)
{
    ENXBView *self;
    self = g_new0(ENXBView, 1);
    self->backend = backend;
    self->view = view;
    self->surface = _enxb_surface_from_weston_surface(self->backend, self->view->surface);

    guint32 selmask =  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    guint32 selval[] = { 0, 0, 1, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE, backend->map };
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *err;

    self->window = xcb_generate_id(self->backend->xcb_connection);
    cookie = xcb_create_window_checked(self->backend->xcb_connection,
                      self->backend->depth,                /* depth         */
                      self->window,
                      self->backend->screen->root,         /* parent window */
                      0, 0,                          /* x, y          */
                      self->surface->size.width,    /* width         */
                      self->surface->size.height,   /* height        */
                      0,                             /* border_width  */
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, /* class         */
                      self->backend->visual->visual_id,    /* visual        */
                      selmask, selval);              /* masks         */
    err = xcb_request_check(self->backend->xcb_connection, cookie);
    if ( err != NULL )
    {
        g_warning("Failed to create window, err: %d", err->error_code);
        free(err);
        g_free(self);
        return NULL;
    }

    self->cairo_surface = cairo_xcb_surface_create(self->backend->xcb_connection, self->window, self->backend->visual, self->surface->size.width, self->surface->size.height);

    self->destroy_listener.notify = _enxb_view_destroy_notify;
    wl_signal_add(&self->view->destroy_signal, &self->destroy_listener);

    g_hash_table_insert(self->backend->views, GINT_TO_POINTER(self->window), self);

    return self;
}

static ENXBView *
_enxb_view_from_weston_view(ENXBBackend *backend, struct weston_view *view)
{
    struct wl_listener *listener;
    ENXBView *self;

    listener = wl_signal_get(&view->destroy_signal, _enxb_view_destroy_notify);
    if ( listener != NULL )
        return wl_container_of(listener, self, destroy_listener);

    return _enxb_view_new(backend, view);
}

static void
_enxb_view_repaint(ENXBView *self)
{
    gfloat x, y;
    weston_view_to_global_float(self->view, 0, 0, &x, &y);

    guint16 mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    guint32 vals[] = { x, y };
    xcb_configure_window(self->backend->xcb_connection, self->window, mask, vals);

    if ( ! self->mapped )
    {
        xcb_map_window(self->backend->xcb_connection, self->window);
        self->mapped = TRUE;
    }

    xcb_clear_area(self->backend->xcb_connection, TRUE, self->window, 0, 0, 0, 0);

    xcb_flush(self->backend->xcb_connection);
}

static int
_enxb_output_enable(struct weston_output *output)
{
    return 0;
}

static int
_enxb_output_disable(struct weston_output *output)
{
    return 0;
}

static int
_enxb_output_switch_mode(struct weston_output *output, struct weston_mode *mode)
{
    return 0;
}

static void
_enxb_output_start_repaint_loop(struct weston_output *output)
{
    struct timespec ts;

    weston_compositor_read_presentation_clock(output->compositor, &ts);
    weston_output_finish_frame(output, &ts, WP_PRESENTATION_FEEDBACK_INVALID);
}


static int
_enxb_output_finish_frame(gpointer user_data)
{
    ENXBOutput *output = user_data;
    struct timespec ts;

    weston_compositor_read_presentation_clock(output->base.compositor, &ts);
    weston_output_finish_frame(&output->base, &ts, WP_PRESENTATION_FEEDBACK_INVALID);
    output->finish_frame_timer = 0;

    return G_SOURCE_REMOVE;
}

static int
_enxb_output_repaint(struct weston_output *woutput, pixman_region32_t *damage, void *repaint_data)
{
    ENXBBackend *backend = wl_container_of(woutput->compositor->backend, backend, base);
    ENXBOutput *output = wl_container_of(woutput, output, base);
    struct weston_view *wview;

    wl_list_for_each_reverse(wview, &backend->compositor->view_list, link)
    {
        ENXBView *view = _enxb_view_from_weston_view(backend, wview);
        if ( view == NULL )
            continue;

        if ( view->view->plane == &backend->compositor->primary_plane )
            _enxb_view_repaint(view);
    }
    wl_signal_emit(&output->base.frame_signal, &output->base);
    output->finish_frame_timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 10, _enxb_output_finish_frame, output, NULL);

    return 0;
}

static void
_enxb_output_destroy(struct weston_output *woutput)
{
    ENXBHead *head = wl_container_of(woutput, head, output.base);

    weston_output_release(&head->output.base);
}

static struct weston_output *
_enxb_output_create(struct weston_compositor *compositor, const char *name)
{
    g_return_val_if_fail(name != NULL, NULL);

    ENXBBackend *backend = wl_container_of(compositor->backend, backend, base);
    ENXBHead *head;

    head = g_hash_table_lookup(backend->heads, name);
    g_return_val_if_fail(head != NULL, NULL);

    weston_output_init(&head->output.base, compositor, name);

    head->output.base.destroy = _enxb_output_destroy;
    head->output.base.enable = _enxb_output_enable;
    head->output.base.disable = _enxb_output_disable;
    head->output.base.switch_mode = _enxb_output_switch_mode;
    head->output.base.attach_head = NULL;
    head->output.base.start_repaint_loop = _enxb_output_start_repaint_loop;
    head->output.base.repaint = _enxb_output_repaint;

    return &head->output.base;
}

static ENXBHead *
_enxb_head_new(ENXBBackend *backend, const gchar *name)
{
    ENXBHead *head;
    struct weston_output *woutput;

    head = g_new0(ENXBHead, 1);

    weston_head_init(&head->base, name);
    weston_head_set_connection_status(&head->base, true);
    weston_compositor_add_head(backend->compositor, &head->base);

    g_hash_table_insert(backend->heads, head->base.name, head);

    woutput = weston_compositor_create_output_with_head(backend->compositor, &head->base);
    g_return_val_if_fail(woutput == &head->output.base, NULL);

    weston_head_set_monitor_strings(&head->base, "X11", name, NULL);

    head->mode.flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
    head->mode.refresh = 60000;

    wl_list_insert(&head->output.base.mode_list, &head->mode.link);
    head->output.base.current_mode = &head->mode;
    head->output.base.native_mode = &head->mode;

    weston_output_set_transform(&head->output.base, WL_OUTPUT_TRANSFORM_NORMAL);
    weston_output_set_scale(&head->output.base, 1);

    weston_output_enable(&head->output.base);

    return head;
}

static void
_enxb_head_free(gpointer data)
{
    ENXBHead *head = data;

    weston_output_destroy(&head->output.base);
    weston_head_release(&head->base);
    g_free(head);
}

static inline gint
_enxb_compute_scale_from_dpi(gdouble dpi)
{
    return (gint) ( dpi / 96. + 0.25 );
}

static inline gint
_enxb_compute_scale_from_size(gint w, gint h, gint mm_w, gint mm_h)
{
    gdouble dpi_x = ( (gdouble) w * 25.4 ) / (gdouble) mm_w;
    gdouble dpi_y = ( (gdouble) h * 25.4 ) / (gdouble) mm_h;
    gdouble dpi = MIN(dpi_x, dpi_y);

    return _enxb_compute_scale_from_dpi(dpi);
}

static void
_enxb_head_update(ENXBBackend *backend, xcb_randr_get_output_info_reply_t *output, xcb_randr_get_crtc_info_reply_t *crtc)
{
    ENXBHead *head;
    gchar *name;
    gsize l = xcb_randr_get_output_info_name_length(output) + 1;

    name = g_newa(gchar, l);
    g_snprintf(name, l, "%s", (const gchar *) xcb_randr_get_output_info_name(output));

    head = g_hash_table_lookup(backend->heads, name);
    if ( head == NULL )
        head = _enxb_head_new(backend, name);

    head->mode.width = crtc->width;
    head->mode.height = crtc->height;

    weston_head_set_physical_size(&head->base, output->mm_width, output->mm_height);
    /* TODO: use crtc transform */
    weston_output_set_transform(&head->output.base, WL_OUTPUT_TRANSFORM_NORMAL);
    weston_output_mode_set_native(&head->output.base, &head->mode, _enxb_compute_scale_from_size(crtc->width, crtc->height, output->mm_width, output->mm_height));
    weston_output_move(&head->output.base, crtc->x, crtc->y);
}

static void
_enxb_backend_check_outputs(ENXBBackend *backend)
{
    xcb_randr_get_screen_resources_current_cookie_t rcookie;
    xcb_randr_get_screen_resources_current_reply_t *ressources;

    rcookie = xcb_randr_get_screen_resources_current(backend->xcb_connection, backend->screen->root);
    if ( ( ressources = xcb_randr_get_screen_resources_current_reply(backend->xcb_connection, rcookie, NULL) ) == NULL )
    {
        g_warning("Couldn't get RandR screen ressources");
        return;
    }

    xcb_timestamp_t cts;
    xcb_randr_output_t *randr_outputs;
    gint i, length;

    cts = ressources->config_timestamp;

    length = xcb_randr_get_screen_resources_current_outputs_length(ressources);
    randr_outputs = xcb_randr_get_screen_resources_current_outputs(ressources);

    GHashTableIter iter;
    ENXBHead *head;
    g_hash_table_iter_init(&iter, backend->heads);
    while ( g_hash_table_iter_next(&iter, NULL, (gpointer *) &head) )
        weston_head_set_connection_status(&head->base, false);

    xcb_randr_get_output_info_reply_t *output;
    xcb_randr_get_crtc_info_reply_t *crtc;
    for ( i = 0 ; i < length ; ++i )
    {
        xcb_randr_get_output_info_cookie_t ocookie;

        ocookie = xcb_randr_get_output_info(backend->xcb_connection, randr_outputs[i], cts);
        if ( ( output = xcb_randr_get_output_info_reply(backend->xcb_connection, ocookie, NULL) ) == NULL )
            continue;

        xcb_randr_get_crtc_info_cookie_t ccookie;

        ccookie = xcb_randr_get_crtc_info(backend->xcb_connection, output->crtc, cts);
        if ( ( crtc = xcb_randr_get_crtc_info_reply(backend->xcb_connection, ccookie, NULL) ) != NULL )
        {
            _enxb_head_update(backend, output, crtc);
            free(crtc);
        }
        free(output);
    }

    g_hash_table_iter_init(&iter, backend->heads);
    while ( g_hash_table_iter_next(&iter, NULL, (gpointer *) &head) )
    {
        if ( ! weston_head_is_connected(&head->base) )
            g_hash_table_iter_remove(&iter);
    }
}

static gboolean
_enxb_backend_event_callback(xcb_generic_event_t *event, gpointer user_data)
{
    ENXBBackend *backend = user_data;

    if ( event == NULL )
    {
        weston_compositor_exit_with_code(backend->compositor, 2);
        return G_SOURCE_REMOVE;
    }

    gint type = event->response_type & ~0x80;

    /* RandR events */
    if ( backend->randr )
    switch ( type - backend->randr_event_base )
    {
    case XCB_RANDR_SCREEN_CHANGE_NOTIFY:
        _enxb_backend_check_outputs(backend);
        return G_SOURCE_CONTINUE;
    case XCB_RANDR_NOTIFY:
        return G_SOURCE_CONTINUE;
    default:
    break;
    }

    if ( backend->xkb && ( type == backend->xkb_event_base ) )
    switch ( event->pad0 )
    {
    case XCB_XKB_MAP_NOTIFY:
    {
        struct xkb_keymap *keymap = xkb_x11_keymap_new_from_device(backend->xkb_context, backend->xcb_connection, backend->xkb_device_id, XKB_KEYMAP_COMPILE_NO_FLAGS);
        weston_seat_update_keymap(&backend->core_seat, keymap);
        xkb_keymap_unref(keymap);
        return G_SOURCE_CONTINUE;
    }
    case XCB_XKB_STATE_NOTIFY:
    {
        xcb_xkb_state_notify_event_t *e = (xcb_xkb_state_notify_event_t *) event;
        struct weston_keyboard *keyboard = weston_seat_get_keyboard(&backend->core_seat);
        xkb_state_update_mask(keyboard->xkb_state.state ,e->baseMods, e->latchedMods, e->lockedMods, e->baseGroup, e->latchedGroup, e->lockedGroup);

        notify_modifiers(&backend->core_seat, wl_display_next_serial(backend->compositor->wl_display));
        return G_SOURCE_CONTINUE;
    }
    }

    /* XFixes events */
    if ( backend->xfixes )
    switch ( type - backend->xfixes_event_base )
    {
    case XCB_XFIXES_SELECTION_NOTIFY:
    {
        xcb_xfixes_selection_notify_event_t *e = (xcb_xfixes_selection_notify_event_t *)event;
        if ( e->selection == backend->ewmh._NET_WM_CM_Sn[backend->screen_number] )
        {
            gboolean compositing = ( e->owner != XCB_WINDOW_NONE );
            if ( backend->compositing != compositing )
                backend->compositing = compositing;
        }

        return G_SOURCE_CONTINUE;
    }
    break;
    }

    /* Core events */
    switch ( type )
    {
    case XCB_EXPOSE:
    {
        xcb_expose_event_t *e = (xcb_expose_event_t *)event;
        ENXBView *view;

        view = g_hash_table_lookup(backend->views, GINT_TO_POINTER(e->window));
        if ( view == NULL )
            break;

        cairo_t *cr;
        cr = cairo_create(view->cairo_surface);
        cairo_set_source_surface(cr, view->surface->cairo_surface, 0, 0);
        cairo_rectangle(cr, e->x, e->y, e->width, e->height);
        cairo_clip(cr);
        if ( view->view->alpha < 1.0 )
            cairo_paint_with_alpha(cr, view->view->alpha);
        else
            cairo_paint(cr);
        cairo_destroy(cr);
        xcb_flush(backend->xcb_connection);
    }
    break;
    case XCB_BUTTON_PRESS:
    {
        //xcb_button_press_event_t *e = (xcb_button_press_event_t *)event;
    }
    break;
    case XCB_BUTTON_RELEASE:
    {
        //xcb_button_release_event_t *e = (xcb_button_release_event_t *)event;
    }
    break;
    case XCB_PROPERTY_NOTIFY:
    {
    }
    break;
    default:
    break;
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
_enxb_get_colormap(ENXBBackend *backend)
{
    gboolean ret = FALSE;

    backend->visual = xcb_aux_find_visual_by_attrs(backend->screen, XCB_VISUAL_CLASS_DIRECT_COLOR, 32);
    if ( backend->visual == NULL )
        backend->visual = xcb_aux_find_visual_by_attrs(backend->screen, XCB_VISUAL_CLASS_TRUE_COLOR, 32);

    if ( backend->visual != NULL )
    {
        xcb_void_cookie_t c;
        xcb_generic_error_t *e;
        backend->map = xcb_generate_id(backend->xcb_connection);
        c = xcb_create_colormap_checked(backend->xcb_connection, ( backend->visual->_class == XCB_VISUAL_CLASS_DIRECT_COLOR) ? XCB_COLORMAP_ALLOC_ALL : XCB_COLORMAP_ALLOC_NONE, backend->map, backend->screen->root, backend->visual->visual_id);
        e = xcb_request_check(backend->xcb_connection, c);
        if ( e == NULL )
            ret = TRUE;
        else
        {
            xcb_free_colormap(backend->xcb_connection, backend->map);
            free(e);
        }
    }

    if ( ! ret )
    {
        backend->visual = xcb_aux_find_visual_by_id(backend->screen, backend->screen->root_visual);
        backend->map = backend->screen->default_colormap;
    }

    backend->depth = xcb_aux_get_depth_of_visual(backend->screen, backend->visual->visual_id);
    return ret;
}

static void
_enxb_backend_destroy(struct weston_compositor *compositor)
{
    ENXBBackend *backend = wl_container_of(compositor->backend, backend, base);

    if ( backend->custom_map )
        xcb_free_colormap(backend->xcb_connection, backend->map);

    g_hash_table_unref(backend->views);
    g_hash_table_unref(backend->heads);

    g_water_xcb_source_free(backend->source);

    g_free(backend);
}

static gboolean
_enxb_backend_init(struct weston_compositor *compositor, ENXBBackendConfig *config)
{
    ENXBBackend *backend = g_new0(ENXBBackend, 1);
    backend->compositor = compositor;
    memcpy(&backend->config, config, config->base.struct_size);
    backend->compositor->backend = &backend->base;

    backend->base.create_output = _enxb_output_create;
    backend->base.destroy = _enxb_backend_destroy;

    const xcb_query_extension_reply_t *extension_query;
    gint screen;
    backend->source = g_water_xcb_source_new(NULL, NULL, &screen, _enxb_backend_event_callback, backend, NULL);
    if ( backend->source == NULL )
        goto fail;

    backend->xcb_connection = g_water_xcb_source_get_connection(backend->source);
    backend->screen_number = screen;
    backend->screen = xcb_aux_get_screen(backend->xcb_connection, screen);

    xcb_intern_atom_cookie_t *ac;
    ac = xcb_ewmh_init_atoms(backend->xcb_connection, &backend->ewmh);
    xcb_ewmh_init_atoms_replies(&backend->ewmh, ac, NULL);

    extension_query = xcb_get_extension_data(backend->xcb_connection, &xcb_randr_id);
    if ( ! extension_query->present )
    {
        g_warning("No RandR extension");
        goto fail;
    }
    backend->randr_event_base = extension_query->first_event;
    xcb_randr_select_input(backend->xcb_connection, backend->screen->root,
            XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
            XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
            XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
            XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY);


    weston_seat_init(&backend->core_seat, backend->compositor, "default");
    weston_seat_init_pointer(&backend->core_seat);

    if ( xkb_x11_setup_xkb_extension(backend->xcb_connection, XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, NULL, NULL, &backend->xkb_event_base, NULL) > -1 )
    {
        backend->xkb_device_id = xkb_x11_get_core_keyboard_device_id(backend->xcb_connection);

        enum
        {
            required_events =
                ( XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY |
                  XCB_XKB_EVENT_TYPE_MAP_NOTIFY |
                  XCB_XKB_EVENT_TYPE_STATE_NOTIFY ),

            required_nkn_details =
                ( XCB_XKB_NKN_DETAIL_KEYCODES ),

            required_map_parts   =
                ( XCB_XKB_MAP_PART_KEY_TYPES |
                  XCB_XKB_MAP_PART_KEY_SYMS |
                  XCB_XKB_MAP_PART_MODIFIER_MAP |
                  XCB_XKB_MAP_PART_EXPLICIT_COMPONENTS |
                  XCB_XKB_MAP_PART_KEY_ACTIONS |
                  XCB_XKB_MAP_PART_VIRTUAL_MODS |
                  XCB_XKB_MAP_PART_VIRTUAL_MOD_MAP ),

            required_state_details =
                ( XCB_XKB_STATE_PART_MODIFIER_BASE |
                  XCB_XKB_STATE_PART_MODIFIER_LATCH |
                  XCB_XKB_STATE_PART_MODIFIER_LOCK |
                  XCB_XKB_STATE_PART_GROUP_BASE |
                  XCB_XKB_STATE_PART_GROUP_LATCH |
                  XCB_XKB_STATE_PART_GROUP_LOCK ),
        };

        static const xcb_xkb_select_events_details_t details = {
            .affectNewKeyboard  = required_nkn_details,
            .newKeyboardDetails = required_nkn_details,
            .affectState        = required_state_details,
            .stateDetails       = required_state_details,
        };
        xcb_xkb_select_events(backend->xcb_connection, backend->xkb_device_id, required_events, 0, required_events, required_map_parts, required_map_parts, &details);

        backend->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap *keymap = xkb_x11_keymap_new_from_device(backend->xkb_context, backend->xcb_connection, backend->xkb_device_id, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if ( keymap != NULL )
        {
            weston_seat_init_keyboard(&backend->core_seat, keymap);
            backend->xkb = TRUE;
            xkb_keymap_unref(keymap);
        }
    }

    backend->custom_map = _enxb_get_colormap(backend);

    if ( backend->custom_map )
    {
        /* We have a 32bit color map, try to support compositing */
        xcb_get_selection_owner_cookie_t oc;
        xcb_window_t owner;
        oc = xcb_ewmh_get_wm_cm_owner(&backend->ewmh, backend->screen_number);
        backend->compositing = xcb_ewmh_get_wm_cm_owner_reply(&backend->ewmh, oc, &owner, NULL) && ( owner != XCB_WINDOW_NONE );

        extension_query = xcb_get_extension_data(backend->xcb_connection, &xcb_xfixes_id);
        if ( ! extension_query->present )
            g_warning("No XFixes extension");
        else
        {
            xcb_xfixes_query_version_cookie_t vc;
            xcb_xfixes_query_version_reply_t *r;
            vc = xcb_xfixes_query_version(backend->xcb_connection, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);
            r = xcb_xfixes_query_version_reply(backend->xcb_connection, vc, NULL);
            if ( r == NULL )
                g_warning("Cannot get XFixes version");
            else
            {
                backend->xfixes = TRUE;
                backend->xfixes_event_base = extension_query->first_event;
                xcb_xfixes_select_selection_input_checked(backend->xcb_connection, backend->screen->root,
                    backend->ewmh._NET_WM_CM_Sn[backend->screen_number],
                    XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
                    XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
                    XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
            }
        }
    }

    xcb_flush(backend->xcb_connection);

    backend->heads = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _enxb_head_free);
    _enxb_backend_check_outputs(backend);

    backend->views = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

    return TRUE;

fail:
    if ( backend->source != NULL )
        g_water_xcb_source_free(backend->source);
    g_free(backend);
    return FALSE;
}

EVENTD_EXPORT int
weston_backend_init(struct weston_compositor *compositor, struct weston_backend_config *config_base)
{

    if ( ( config_base->struct_version != ENXB_BACKEND_CONFIG_VERSION ) || ( config_base->struct_size > sizeof(ENXBBackendConfig) ) )
        return -1;

    if ( ! _enxb_backend_init(compositor, (ENXBBackendConfig *) config_base) )
        return -1;

    compositor->renderer = &_enxb_renderer;

    return 0;
}
