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
#include <string.h>
#include <glib.h>
#include <gmodule.h>
#include <compositor.h>
#include <windowed-output-api.h>
#include "libgwater-wayland-server.h"
#include "backend.h"

typedef struct {
    GWaterWaylandServerSource *source;
    struct wl_display *display;
    GMainLoop *loop;
    struct weston_compositor *compositor;
    ENXBBackendConfig backend_config;
} ENXBContext;

static int
_enxb_log(const char *format, va_list args)
{
    return vfprintf(stderr, format, args);
}

static void
_enxb_load_notification_area(ENXBContext *context)
{
    GModule *mod;
    const gchar *path = WESTON_PLUGINS_DIR G_DIR_SEPARATOR_S "notification-area.so";
    int argc = 0;
    int (*init)(struct weston_compositor *compositor, int *argc, char *argv[]);

    g_debug("Try weston plugin %s", path);
    mod = g_module_open(path, G_MODULE_BIND_LOCAL | G_MODULE_BIND_LAZY);
    if ( mod != NULL )
    {
        if ( ! g_module_symbol(mod, "wet_module_init", (gpointer *) &init) )
        {
            if ( ! g_module_symbol(mod, "module_init", (gpointer *) &init) )
                g_debug("Couldn’t find init function for plugin");
            else if ( init(context->compositor, &argc, NULL) < 0 )
                g_debug("Plugin init failed");
            else
                mod = NULL;

        }
        else if ( init(context->compositor, &argc, NULL) < 0 )
            g_debug("Plugin init failed");
        else
            mod = NULL;
        if ( mod != NULL )
            g_module_close(mod);
    }
    else
        g_debug("Couldn’t load plugin: %s", g_module_error());
}

static void
_enxb_exit(struct weston_compositor *compositor)
{
    ENXBContext *context = weston_compositor_get_user_data(compositor);
    wl_display_terminate(context->display);
}

int
main()
{
    ENXBContext context_ = { .display = NULL }, *context = &context_;

    weston_log_set_handler(_enxb_log, _enxb_log);

    /* Ignore SIGPIPE as it is useless */
    signal(SIGPIPE, SIG_IGN);

    context->source = g_water_wayland_server_source_new(NULL);
    context->display = g_water_wayland_server_source_get_display(context->source);
    context->compositor = weston_compositor_create(context->display, context);

    /* Just using NULL here will use environment variables */
    weston_compositor_set_xkb_rule_names(context->compositor, NULL);

    context->backend_config.base.struct_version = ENXB_BACKEND_CONFIG_VERSION;
    context->backend_config.base.struct_size = sizeof(ENXBBackendConfig);
    g_setenv("WESTON_MODULE_MAP", "x11-backend.so=" BUILD_DIR G_DIR_SEPARATOR_S "eventd-nd-x11-bridge." G_MODULE_SUFFIX, TRUE);
    if ( weston_compositor_load_backend(context->compositor, WESTON_BACKEND_X11, &context->backend_config.base) < 0 )
        return 1;

    context->compositor->vt_switching = 0;
    context->compositor->exit = _enxb_exit;

    _enxb_load_notification_area(context);

    const char *socket_name;
    socket_name = wl_display_add_socket_auto(context->display);
    if ( socket_name == NULL )
    {
        weston_log("Couldn’t add socket: %s\n", strerror(errno));
        return -1;
    }

    weston_compositor_wake(context->compositor);

    context->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(context->loop);
    g_main_loop_unref(context->loop);

    weston_compositor_destroy(context->compositor);

    return 0;
}
