/* 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * File: upstart-dbus.c
 * Copyright (C) 2010 Senko Rasic <senko.rasic@dobarkod.hr>
 * Copyright (c) 2010 Ante Karamatic <ivoks@init.hr>
 *
 *
 * Each exported function is standalone, and creates a new connection to
 * the upstart daemon. This is because lrmd plugins fork off for exec,
 * and if we try and share the connection, the whole thing blocks
 * indefinitely.
 */

#include <crm_internal.h>

#include <stdio.h>

#include <crm/crm.h>
#include <upstart.h>

#include <glib.h>
#include <gio/gio.h>

#define BUS_NAME "com.ubuntu.Upstart"
#define BUS_PATH "/com/ubuntu/Upstart"

#define BUS_MANAGER_IFACE BUS_NAME"0_6"
#define BUS_PROPERTY_IFACE "org.freedesktop.DBus.Properties"

/*
  http://upstart.ubuntu.com/wiki/DBusInterface
*/
static GDBusProxy *upstart_proxy = NULL;

static GDBusProxy *
get_proxy(const char *path, const char *interface) 
{
    GError *error = NULL;
    GDBusProxy *proxy = NULL;
    
    g_type_init();

    if(path == NULL) {
        path = BUS_PATH;
    }

    proxy = g_dbus_proxy_new_for_bus_sync (
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, /* GDBusInterfaceInfo */
        BUS_NAME, path, interface,
        NULL, /* GCancellable */ &error);
    
    if (error) {
        crm_err("Can't connect obtain proxy to %s interface: %s", interface, error->message);
        g_error_free(error);
        proxy = NULL;
    }
    return proxy;
}

static gboolean
upstart_init(void) 
{
    if(upstart_proxy == NULL) {
        upstart_proxy = get_proxy(NULL, BUS_MANAGER_IFACE);
    }
    if(upstart_proxy == NULL) {
        return FALSE;
    }
    return TRUE;
}

void upstart_cleanup(void)
{
    g_object_unref(upstart_proxy);
    upstart_proxy = NULL;
}

static gboolean
upstart_job_by_name (
    GDBusProxy *proxy,
    const gchar *arg_name,
    gchar **out_unit,
    GCancellable *cancellable,
    GError **error)
{
/*
  com.ubuntu.Upstart0_6.GetJobByName (in String name, out ObjectPath job)
*/  
    GVariant *_ret = g_dbus_proxy_call_sync (
        proxy, "GetJobByName", g_variant_new ("(s)", arg_name),
        G_DBUS_CALL_FLAGS_NONE, -1, cancellable, error);

    if (_ret) {
        g_variant_get (_ret, "(o)", out_unit);
        g_variant_unref (_ret);
    }
    
    return _ret != NULL;
}

GList *
upstart_job_listall(void) 
{
    GList *units = NULL;
    GError *error = NULL;
    GVariantIter *iter;
    char *path = NULL;
    GVariant *_ret = NULL;
    int lpc = 0;

    CRM_ASSERT(upstart_init());

/*
  com.ubuntu.Upstart0_6.GetAllJobs (out <Array of ObjectPath> jobs)
*/  
    _ret = g_dbus_proxy_call_sync (
        upstart_proxy, "GetAllJobs", g_variant_new ("()"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (error) {
        crm_err("Call to GetAllJobs failed: %s", error->message);
        g_error_free(error);
    }

    g_variant_get (_ret, "(ao)", &iter);
    while (g_variant_iter_loop (iter, "o", &path)) {
        lpc++;
        crm_trace("%s\n", path);
        units = g_list_append(units, strdup(path));
    }
    crm_info("Call to GetAllJobs passed: type '%s', count %d", g_variant_get_type_string (_ret), lpc);
    
    g_variant_iter_free(iter);
    g_variant_unref(_ret);
    return units;
}

gboolean
upstart_job_exists(const char *name)
{
    char *path = NULL;
    GError *error = NULL;
    gboolean pass = FALSE;

    CRM_ASSERT(upstart_init());
    pass = upstart_job_by_name (upstart_proxy, name, &path, NULL, &error);

    if (error || pass == FALSE) {
        crm_trace("Call to ListUnits failed: %s", error->message);
        g_error_free(error);
        pass = FALSE;

    } else {
        crm_trace("Got %s", path);
    }
    /* free(path) */
    return pass;
}

static char *
upstart_job_property(const char *obj, const gchar *iface, const char *name)
{
    GError *error = NULL;
    GDBusProxy *proxy;
    GVariant *asv = NULL;
    GVariant *value = NULL;
    GVariant *_ret = NULL;
    char *output = NULL;

    crm_info("Calling GetAll on %s", obj);
    proxy = get_proxy(obj, BUS_PROPERTY_IFACE);
    
    _ret = g_dbus_proxy_call_sync (
        proxy, "GetAll", g_variant_new ("(s)", iface),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (error) {
        crm_err("Cannot get properties for %s: %s", g_dbus_proxy_get_object_path(proxy), error->message);
        g_error_free(error);
        g_object_unref(proxy);
        return NULL;
    }
    crm_info("Call to GetAll passed: type '%s' %d\n", g_variant_get_type_string (_ret), g_variant_n_children (_ret));

    asv = g_variant_get_child_value(_ret, 0);
    crm_trace("asv type '%s' %d\n", g_variant_get_type_string (asv), g_variant_n_children (asv));
    
    value = g_variant_lookup_value(asv, name, NULL);
    if(value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
        crm_info("Got value '%s' for %s[%s]", g_variant_get_string(value, NULL), obj, name);
        output = g_variant_dup_string(value, NULL);

    } else {
        crm_info("No value for %s[%s]", obj, name);
    }

    g_object_unref(proxy);
    return output;
}

static char *
get_first_instance(const gchar *job)
{
    char *instance = NULL;
    GError *error = NULL;
    GDBusProxy *proxy = get_proxy(job, BUS_MANAGER_IFACE".Job");
    GVariant *_ret = g_dbus_proxy_call_sync (
        proxy, "GetAllInstances", g_variant_new ("()"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    
    if (error) {
        crm_err("Cannot call GetAllInstances for %s: %s", job, error->message);
        g_error_free(error);
        return NULL;
    }

    crm_trace("Call to GetAllInstances passed: type '%s' %d\n", g_variant_get_type_string (_ret), g_variant_n_children (_ret));
    if(g_variant_n_children (_ret)) {
        GVariant *tmp1 = g_variant_get_child_value(_ret, 0);
        if(g_variant_n_children (tmp1)) {
            GVariant *tmp2 = g_variant_get_child_value(tmp1, 0);
            instance = g_variant_dup_string(tmp2, NULL);
        }
    }
    
    crm_info("Result: %s", instance);
    return instance;
}

gboolean
upstart_job_running(const gchar *name)
{
    char *job = NULL;
    GError *error = NULL;
    gboolean pass = FALSE;

    pass = upstart_job_by_name (upstart_proxy, name, &job, NULL, &error);
    if (error || pass == FALSE) {
        crm_err("Call to ListUnits failed: %s", error->message);
        g_error_free(error);

    } else {
        char *instance = get_first_instance(job);
        pass = FALSE;
        if(instance) {
            if (instance) {
                char *state = upstart_job_property(instance, BUS_MANAGER_IFACE".Instance", "state");
                crm_info("State of %s: %s", name, state);
                if(state) {
                    pass = !g_strcmp0(state, "running");
                }
                free(state);
            }
        }
    }

    crm_info("%s is%s running", name, pass?"":" not");
    return pass;
}

static char *
upstart_job_metadata(const char *name) 
{
    return g_strdup_printf(
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE resource-agent SYSTEM \"ra-api-1.dtd\">\n"
        "<resource-agent name=\"%s\" version=\"0.1\">\n"
        "  <version>1.0</version>\n"
        "  <longdesc lang=\"en\">\n"
        "    Upstart agent for controlling the system %s service"
        "  </longdesc>\n"
        "  <shortdesc lang=\"en\">%s upstart agent</shortdesc>\n"
        "  <parameters>\n"
        "  </parameters>\n"
        "  <actions>\n"
        "    <action name=\"start\"   timeout=\"15\" />\n"
        "    <action name=\"stop\"    timeout=\"15\" />\n"
        "    <action name=\"status\"  timeout=\"15\" />\n"
        "    <action name=\"restart\"  timeout=\"15\" />\n"
        "    <action name=\"monitor\" timeout=\"15\" interval=\"15\" start-delay=\"15\" />\n"
        "    <action name=\"meta-data\"  timeout=\"5\" />\n"
        "  </actions>\n"
        "  <special tag=\"upstart\">\n"
        "  </special>\n"
        "</resource-agent>\n",
        name, name, name);
}

gboolean
upstart_job_exec(svc_action_t* op, gboolean synchronous)
{
    char *job = NULL;
    GError *error = NULL;
    gboolean pass = FALSE;
    gchar *no_args[] = { NULL };
    const char *action = op->action;

    GVariant *_ret = NULL;
    GDBusProxy *job_proxy = NULL;
    
    op->rc = PCMK_EXECRA_UNKNOWN_ERROR;
    CRM_ASSERT(upstart_init());

    pass = upstart_job_by_name (upstart_proxy, op->rsc, &job, NULL, &error);
    if (error || pass == FALSE) {
        crm_err("Call to ListUnits failed: %s", error->message);
        g_error_free(error);
        op->rc = PCMK_EXECRA_NOT_INSTALLED;
        return FALSE;
    }
    
    if (safe_str_eq(op->action, "meta-data")) {
        op->stdout_data = upstart_job_metadata(op->rsc);
        op->rc = PCMK_EXECRA_OK;
        goto cleanup;
    }

    if (safe_str_eq(op->action, "monitor") || safe_str_eq(action, "status")) {
        gboolean running = upstart_job_running (op->rsc);
        crm_trace("%s", running ? "running" : "stopped");
		
        if (running) {
            op->rc = PCMK_EXECRA_OK;
            goto cleanup;
        }
        op->rc = PCMK_EXECRA_NOT_RUNNING;
        goto cleanup;

    } else if (!g_strcmp0(action, "start")) {
        action = "Start";
    } else if (!g_strcmp0(action, "stop")) {
        action = "Stop";
    } else if (!g_strcmp0(action, "restart")) {
        action = "Restart";
    } else {
        return PCMK_EXECRA_UNIMPLEMENT_FEATURE;
    }

    /* TODO: Cache this */
    job_proxy = get_proxy(job, BUS_MANAGER_IFACE".Job");
    
    crm_info("Calling %s for %s: %s", action, op->rsc, job);
    _ret = g_dbus_proxy_call_sync (
        job_proxy, action, g_variant_new ("(^asb)", no_args, TRUE),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    
    if (error) {
        /* ignore "already started" or "not running" errors */
        if (safe_str_eq(action, "Start")
            && strstr(error->message, BUS_MANAGER_IFACE".Error.AlreadyStarted")) {
            crm_trace("Masking Start failure for %s: already started", op->rsc);
            op->rc = PCMK_EXECRA_OK;
        } else if (safe_str_eq(action, "Stop")
                   && strstr(error->message, BUS_MANAGER_IFACE".Error.UnknownInstance")) {
            crm_trace("Masking Stop failure for %s: unknown services are stopped", op->rsc);
            op->rc = PCMK_EXECRA_OK;
        } else {
            crm_err("Could not issue %s for %s: %s (%s)", action, op->rsc, error->message, job);
        }
        g_error_free(error);

    } else {
        char *path = NULL;
        g_variant_get(_ret, "(o)", &path);
        crm_info("Call to %s passed: type '%s' %s", action, g_variant_get_type_string (_ret), path);
        op->rc = PCMK_EXECRA_OK;
    }

  cleanup:
    if(job) {
        free(job);
    }
    if(job_proxy) {
        g_object_unref(job_proxy);
    }
    return op->rc == PCMK_EXECRA_OK;
}
