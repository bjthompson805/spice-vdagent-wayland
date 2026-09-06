/*  clipboard.c - vdagent clipboard handling code

    Copyright 2017 Red Hat, Inc.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <config.h>

#ifdef USE_GTK_FOR_CLIPBOARD
# include <gtk/gtk.h>
# include <gdk/wayland/gdkwayland.h>
# include <gio/gio.h>
# include <gio/gunixinputstream.h>
# include <string.h>
# include <syslog.h>
# include <unistd.h>

# include "vdagentd-proto.h"
# include "spice/vd_agent.h"
# include "wlr-protocol/wlr-data-control-unstable-v1-client-protocol.h"
#endif

#include "clipboard.h"

#ifdef USE_GTK_FOR_CLIPBOARD
/* GTK4 port notes (Phase 1+2: text, both CLIPBOARD and PRIMARY selections
 * -- see the project plan for Phase 3, which adds image formats on top of
 * this same structure).
 *
 * Two entirely different mechanisms are used for the two directions, and
 * that split is deliberate, not incidental:
 *
 * - Host has new data, guest should see it (vdagent_clipboard_grab, "the
 *   write side"): uses GTK4's ordinary GdkClipboard (gdk_clipboard_set_
 *   content + VdagentClipboardProvider below). This works fine as-is --
 *   *offering* a selection has never been the problem here.
 *
 * - Guest copies something, host should see it ("the observe side"): does
 *   NOT use GdkClipboard at all. Per the Wayland protocol specification
 *   (not a GDK or Hyprland bug -- confirmed by reading both GDK's and
 *   Hyprland's actual source), a client only receives wl_data_device
 *   selection-changed notifications while it holds keyboard focus. vdagent
 *   is a headless background daemon with no window a user would ever
 *   focus, so it structurally can never observe another client's clipboard
 *   change through the standard wl_data_device protocol GdkClipboard is
 *   built on -- confirmed via GDK_DEBUG=clipboard,events: the offer's
 *   mime-type list came back empty every time because the compositor
 *   never routed the full data_offer event sequence to an unfocused
 *   client, exactly as the spec allows.
 *
 *   The fix is a different, purpose-built Wayland protocol:
 *   wlr-data-control-unstable-v1 (zwlr_data_control_manager_v1 and
 *   friends), designed specifically for focus-independent clipboard
 *   observation -- it's what clipboard managers and `wl-paste --watch`
 *   actually use, for exactly this reason. See data_control_* below.
 */
/* VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD / _PRIMARY are 0 / 1, matching
 * these indices directly. */
#define SELECTION_COUNT 2

#define TEXT_MIME_TYPE "text/plain;charset=utf-8"

enum {
    OWNER_NONE,
    OWNER_GUEST,
    OWNER_CLIENT
};

typedef struct {
    /* write side (host -> guest): ordinary GdkClipboard */
    GdkClipboard *clipboard;
    guint         owner;
    GList        *requests_from_apps; /* GTask* list: VDAgent --> Client (guest paste of our data) */

    /* observe side (guest -> host): wlr-data-control, see above.
     * pending_offer/pending_has_text live on VDAgentClipboards, not here
     * -- a freshly-introduced offer (data_offer event) doesn't say which
     * selection it's for until the following selection/primary_selection
     * event arrives, so there's nothing to key a per-Selection pending
     * state on yet. */
    struct zwlr_data_control_offer_v1 *current_offer; /* finalized; safe to receive() from */
    gboolean      current_has_text;
    /* zwlr_data_control_device_v1's "selection" event fires for every
     * seat-wide selection change, including ones this same process just
     * caused via gdk_clipboard_set_content() on the *other* protocol
     * object (regular wl_data_device, via GDK) -- there's no is_local()
     * equivalent for data-control the way GdkClipboard has, so it can't
     * tell "the guest just copied something" apart from "we just wrote
     * the host's clipboard" on its own. Set right before every
     * gdk_clipboard_set_content() call and consumed by the very next
     * data_control_selection callback, which is that write's own echo. */
    gboolean      expect_own_selection;
} Selection;

#define VDAGENT_TYPE_CLIPBOARD_PROVIDER (vdagent_clipboard_provider_get_type())
G_DECLARE_FINAL_TYPE(VdagentClipboardProvider, vdagent_clipboard_provider,
                     VDAGENT, CLIPBOARD_PROVIDER, GdkContentProvider)

struct _VdagentClipboardProvider {
    GdkContentProvider parent;
    VDAgentClipboards *clipboards; /* borrowed; outlives every provider it creates */
    guint sel_id;
};

G_DEFINE_FINAL_TYPE(VdagentClipboardProvider, vdagent_clipboard_provider, GDK_TYPE_CONTENT_PROVIDER)

static GdkContentFormats *vdagent_clipboard_provider_ref_formats(GdkContentProvider *provider)
{
    (void)provider;
    GdkContentFormatsBuilder *builder = gdk_content_formats_builder_new();
    gdk_content_formats_builder_add_mime_type(builder, TEXT_MIME_TYPE);
    return gdk_content_formats_builder_free_to_formats(builder);
}

/* Defined further down, once struct _VDAgentClipboards (which this
 * dereferences) is actually complete. */
static void vdagent_clipboard_provider_write_mime_type_async(
    GdkContentProvider *provider, const char *mime_type, GOutputStream *stream,
    int io_priority, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);

static gboolean vdagent_clipboard_provider_write_mime_type_finish(
    GdkContentProvider *provider, GAsyncResult *result, GError **error)
{
    (void)provider;
    return g_task_propagate_boolean(G_TASK(result), error);
}

static void vdagent_clipboard_provider_class_init(VdagentClipboardProviderClass *klass)
{
    GdkContentProviderClass *provider_class = GDK_CONTENT_PROVIDER_CLASS(klass);
    provider_class->ref_formats = vdagent_clipboard_provider_ref_formats;
    provider_class->write_mime_type_async = vdagent_clipboard_provider_write_mime_type_async;
    provider_class->write_mime_type_finish = vdagent_clipboard_provider_write_mime_type_finish;
}

static void vdagent_clipboard_provider_init(VdagentClipboardProvider *self)
{
    (void)self;
}
#endif

struct _VDAgentClipboards {
    GObject parent;

#ifdef USE_GTK_FOR_CLIPBOARD
    UdscsConnection *conn;

    Selection selections[SELECTION_COUNT];

    struct wl_display *wl_display; /* borrowed from GDK; not ours to destroy */
    struct wl_registry *wl_registry;
    struct zwlr_data_control_manager_v1 *data_control_manager;
    struct zwlr_data_control_device_v1 *data_control_device;

    /* being built for whichever offer was most recently introduced by a
     * data_offer event; see the Selection typedef above for why this
     * isn't keyed per-selection. */
    struct zwlr_data_control_offer_v1 *pending_offer;
    gboolean pending_has_text;
#else
    struct vdagent_x11 *x11;
#endif
};

struct _VDAgentClipboardsClass
{
    GObjectClass parent;
};

G_DEFINE_TYPE(VDAgentClipboards, vdagent_clipboards, G_TYPE_OBJECT)

#ifdef USE_GTK_FOR_CLIPBOARD
static void vdagent_clipboard_provider_write_mime_type_async(
    GdkContentProvider *provider, const char *mime_type, GOutputStream *stream,
    int io_priority, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    (void)io_priority;
    VdagentClipboardProvider *self = VDAGENT_CLIPBOARD_PROVIDER(provider);
    VDAgentClipboards *c = self->clipboards;
    Selection *sel = &c->selections[self->sel_id];

    GTask *task = g_task_new(provider, cancellable, callback, user_data);

    if (g_strcmp0(mime_type, TEXT_MIME_TYPE) != 0) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "unsupported mime type %s", mime_type);
        g_object_unref(task);
        return;
    }

    g_task_set_task_data(task, g_object_ref(stream), g_object_unref);
    sel->requests_from_apps = g_list_append(sel->requests_from_apps, task);

    udscs_write(c->conn, VDAGENTD_CLIPBOARD_REQUEST, self->sel_id,
               VD_AGENT_CLIPBOARD_UTF8_TEXT, NULL, 0);
}

/* Cancel every pending write-side request and mark no one owns the
 * selection any more -- called whenever ownership is about to change out
 * from under in-flight requests, so nothing completes against stale state.
 */
static void clipboard_new_owner(VDAgentClipboards *c, guint sel_id, guint new_owner)
{
    Selection *sel = &c->selections[sel_id];
    GList *l;

    for (l = sel->requests_from_apps; l != NULL; l = l->next) {
        GTask *task = l->data;
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "clipboard ownership changed");
        g_object_unref(task);
    }
    g_clear_pointer(&sel->requests_from_apps, g_list_free);

    sel->owner = new_owner;
}

/* ---- observe side: wlr-data-control (see the file-level comment) ---- */

static void data_control_offer_offer(void *data, struct zwlr_data_control_offer_v1 *offer,
                                      const char *mime_type)
{
    VDAgentClipboards *c = data;
    (void)offer;
    syslog(LOG_DEBUG, "%s: mime_type=%s", __func__, mime_type);
    if (g_strcmp0(mime_type, TEXT_MIME_TYPE) == 0) {
        c->pending_has_text = TRUE;
    }
}

static const struct zwlr_data_control_offer_v1_listener offer_listener = {
    data_control_offer_offer,
};

static void data_control_data_offer(void *data, struct zwlr_data_control_device_v1 *device,
                                     struct zwlr_data_control_offer_v1 *offer)
{
    VDAgentClipboards *c = data;
    (void)device;

    syslog(LOG_DEBUG, "%s: offer=%p", __func__, (void *)offer);
    c->pending_offer = offer;
    c->pending_has_text = FALSE;
    zwlr_data_control_offer_v1_add_listener(offer, &offer_listener, c);
}

/* Shared by data_control_selection (CLIPBOARD) and
 * data_control_primary_selection (PRIMARY) -- same finalize logic once
 * the sel_id is known, just applied to a different Selection slot. */
static void data_control_offer_finalized(VDAgentClipboards *c, guint sel_id,
                                          struct zwlr_data_control_offer_v1 *offer)
{
    Selection *sel = &c->selections[sel_id];

    syslog(LOG_DEBUG, "%s: sel_id=%u offer=%p expect_own=%d pending_has_text=%d",
           __func__, sel_id, (void *)offer, sel->expect_own_selection, c->pending_has_text);

    if (sel->current_offer) {
        zwlr_data_control_offer_v1_destroy(sel->current_offer);
        sel->current_offer = NULL;
        sel->current_has_text = FALSE;
    }

    if (sel->expect_own_selection) {
        /* echo of our own vdagent_clipboard_grab() -- not a real guest
         * change. We still have to consume/destroy the offer object (it's
         * real, just uninteresting), but must not treat it as GUEST
         * taking ownership. */
        sel->expect_own_selection = FALSE;
        if (offer) {
            zwlr_data_control_offer_v1_destroy(offer);
        }
        return;
    }

    if (!offer) {
        /* guest cleared its clipboard */
        if (sel->owner == OWNER_GUEST) {
            clipboard_new_owner(c, sel_id, OWNER_NONE);
            udscs_write(c->conn, VDAGENTD_CLIPBOARD_RELEASE, sel_id, 0, NULL, 0);
        }
        return;
    }

    /* This offer was introduced by the data_offer event immediately
     * preceding this one, so it must still be our pending one. */
    sel->current_offer = offer;
    sel->current_has_text = c->pending_has_text;
    c->pending_offer = NULL;

    if (!sel->current_has_text) {
        return; /* nothing in a format we support (yet) */
    }

    clipboard_new_owner(c, sel_id, OWNER_GUEST);
    guint32 types[] = {VD_AGENT_CLIPBOARD_UTF8_TEXT};
    udscs_write(c->conn, VDAGENTD_CLIPBOARD_GRAB, sel_id, 0,
               (guint8 *)types, sizeof(types));
}

static void data_control_selection(void *data, struct zwlr_data_control_device_v1 *device,
                                    struct zwlr_data_control_offer_v1 *offer)
{
    (void)device;
    syslog(LOG_DEBUG, "%s: fired, offer=%p", __func__, (void *)offer);
    data_control_offer_finalized(data, VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD, offer);
}

static void data_control_primary_selection(void *data, struct zwlr_data_control_device_v1 *device,
                                            struct zwlr_data_control_offer_v1 *offer)
{
    (void)device;
    syslog(LOG_DEBUG, "%s: fired, offer=%p", __func__, (void *)offer);
    data_control_offer_finalized(data, VD_AGENT_CLIPBOARD_SELECTION_PRIMARY, offer);
}

static void data_control_finished(void *data, struct zwlr_data_control_device_v1 *device)
{
    /* Compositor tore down our data-control device (e.g. seat removed).
     * Nothing to reconnect to for the process's remaining lifetime. */
    (void)data;
    (void)device;
}

static const struct zwlr_data_control_device_v1_listener device_listener = {
    data_control_data_offer,
    data_control_selection,
    data_control_finished,
    data_control_primary_selection,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    VDAgentClipboards *c = data;
    if (g_strcmp0(interface, "zwlr_data_control_manager_v1") == 0) {
        c->data_control_manager = wl_registry_bind(registry, name,
                                                    &zwlr_data_control_manager_v1_interface,
                                                    MIN(version, 2));
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};

typedef struct {
    VDAgentClipboards *c;
    guint sel_id;
} ReceiveRequest;

/* Data lands in a pipe from zwlr_data_control_offer_v1_receive(); read it
 * into a growable buffer and hand it to the client. */
static void data_control_splice_ready_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ReceiveRequest *req = user_data;
    VDAgentClipboards *c = req->c;
    guint sel_id = req->sel_id;
    g_free(req);
    GError *error = NULL;
    GOutputStream *sink = G_OUTPUT_STREAM(source);

    gssize spliced = g_output_stream_splice_finish(sink, result, &error);
    if (spliced < 0) {
        syslog(LOG_WARNING, "%s: sel_id=%u: %s", __func__, sel_id,
               error ? error->message : "splice failed");
        g_clear_error(&error);
        udscs_write(c->conn, VDAGENTD_CLIPBOARD_DATA, sel_id,
                   VD_AGENT_CLIPBOARD_NONE, NULL, 0);
        g_object_unref(sink);
        return;
    }

    gpointer data = g_memory_output_stream_get_data(G_MEMORY_OUTPUT_STREAM(sink));
    gsize size = g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(sink));
    udscs_write(c->conn, VDAGENTD_CLIPBOARD_DATA, sel_id, VD_AGENT_CLIPBOARD_UTF8_TEXT, data, size);
    g_object_unref(sink);
}
#endif

void vdagent_clipboard_grab(VDAgentClipboards *c, guint sel_id,
                            guint32 *types, guint n_types)
{
#ifndef USE_GTK_FOR_CLIPBOARD
    vdagent_x11_clipboard_grab(c->x11, sel_id, types, n_types);
#else
    g_return_if_fail(sel_id < SELECTION_COUNT);

    gboolean text_supported = FALSE;
    for (guint i = 0; i < n_types; i++) {
        if (types[i] == VD_AGENT_CLIPBOARD_UTF8_TEXT) {
            text_supported = TRUE;
            break;
        }
    }
    if (!text_supported) {
        syslog(LOG_WARNING, "%s: sel_id=%u: no supported type offered", __func__, sel_id);
        return;
    }

    Selection *sel = &c->selections[sel_id];
    VdagentClipboardProvider *provider =
        g_object_new(VDAGENT_TYPE_CLIPBOARD_PROVIDER, NULL);
    provider->clipboards = c;
    provider->sel_id = sel_id;

    sel->expect_own_selection = TRUE;
    gdk_clipboard_set_content(sel->clipboard, GDK_CONTENT_PROVIDER(provider));
    g_object_unref(provider);
    clipboard_new_owner(c, sel_id, OWNER_CLIENT);
#endif
}

void vdagent_clipboard_data(VDAgentClipboards *c, guint sel_id,
                            guint type, guchar *data, guint size)
{
#ifndef USE_GTK_FOR_CLIPBOARD
    vdagent_x11_clipboard_data(c->x11, sel_id, type, data, size);
#else
    g_return_if_fail(sel_id < SELECTION_COUNT);
    Selection *sel = &c->selections[sel_id];

    if (sel->requests_from_apps == NULL) {
        syslog(LOG_WARNING, "%s: sel_id=%u: no pending request, skipping", __func__, sel_id);
        return;
    }
    GTask *task = sel->requests_from_apps->data;
    sel->requests_from_apps = g_list_delete_link(sel->requests_from_apps, sel->requests_from_apps);

    if (type != VD_AGENT_CLIPBOARD_UTF8_TEXT) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "client returned unexpected type %u", type);
        g_object_unref(task);
        return;
    }

    GOutputStream *stream = g_task_get_task_data(task);
    GError *error = NULL;
    gboolean ok = g_output_stream_write_all(stream, data, size, NULL,
                                            g_task_get_cancellable(task), &error);
    if (ok) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, error);
    }
    g_object_unref(task);
#endif
}

void vdagent_clipboard_release(VDAgentClipboards *c, guint sel_id)
{
#ifndef USE_GTK_FOR_CLIPBOARD
    vdagent_x11_clipboard_release(c->x11, sel_id);
#else
    g_return_if_fail(sel_id < SELECTION_COUNT);
    if (c->selections[sel_id].owner != OWNER_CLIENT)
        return;

    clipboard_new_owner(c, sel_id, OWNER_NONE);
    gdk_clipboard_set_content(c->selections[sel_id].clipboard, NULL);
#endif
}

void vdagent_clipboards_release_all(VDAgentClipboards *c)
{
#ifndef USE_GTK_FOR_CLIPBOARD
    vdagent_x11_client_disconnected(c->x11);
#else
    guint sel_id, owner;

    for (sel_id = 0; sel_id < SELECTION_COUNT; sel_id++) {
        owner = c->selections[sel_id].owner;
        clipboard_new_owner(c, sel_id, OWNER_NONE);
        if (owner == OWNER_CLIENT)
            gdk_clipboard_set_content(c->selections[sel_id].clipboard, NULL);
        else if (owner == OWNER_GUEST && c->conn)
            udscs_write(c->conn, VDAGENTD_CLIPBOARD_RELEASE, sel_id, 0, NULL, 0);
    }
#endif
}

void vdagent_clipboard_request(VDAgentClipboards *c, guint sel_id, guint type)
{
#ifndef USE_GTK_FOR_CLIPBOARD
    vdagent_x11_clipboard_request(c->x11, sel_id, type);
#else
    Selection *sel;

    if (sel_id >= SELECTION_COUNT || type != VD_AGENT_CLIPBOARD_UTF8_TEXT)
        goto err;
    sel = &c->selections[sel_id];
    if (sel->owner != OWNER_GUEST || !sel->current_offer) {
        syslog(LOG_WARNING, "%s: sel_id=%d: received request "
                            "while not owning clipboard", __func__, sel_id);
        goto err;
    }

    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        syslog(LOG_WARNING, "%s: sel_id=%d: pipe() failed", __func__, sel_id);
        goto err;
    }
    zwlr_data_control_offer_v1_receive(sel->current_offer, TEXT_MIME_TYPE, pipe_fds[1]);
    close(pipe_fds[1]);
    wl_display_flush(c->wl_display); /* must reach the compositor before the source writes */

    GInputStream *src = g_unix_input_stream_new(pipe_fds[0], TRUE);
    GOutputStream *sink = g_memory_output_stream_new_resizable();
    ReceiveRequest *req = g_new(ReceiveRequest, 1);
    req->c = c;
    req->sel_id = sel_id;
    g_output_stream_splice_async(sink, src, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE,
                                 G_PRIORITY_DEFAULT, NULL, data_control_splice_ready_cb, req);
    g_object_unref(src);
    return;
err:
    udscs_write(c->conn, VDAGENTD_CLIPBOARD_DATA, sel_id,
                VD_AGENT_CLIPBOARD_NONE, NULL, 0);
#endif
}

static void
vdagent_clipboards_init(VDAgentClipboards *self)
{
}

VDAgentClipboards *vdagent_clipboards_new(struct vdagent_x11 *x11)
{
    VDAgentClipboards *self = g_object_new(VDAGENT_TYPE_CLIPBOARDS, NULL);

#ifndef USE_GTK_FOR_CLIPBOARD
    self->x11 = x11;
#else
    (void)x11;
    GdkDisplay *gdk_display = gdk_display_get_default();

    /* write side only -- see the file-level comment for why the observe
     * side doesn't use GdkClipboard at all. */
    self->selections[VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD].clipboard =
        gdk_display_get_clipboard(gdk_display);
    self->selections[VD_AGENT_CLIPBOARD_SELECTION_PRIMARY].clipboard =
        gdk_display_get_primary_clipboard(gdk_display);

    /* observe side: bind wlr-data-control directly (no GDK equivalent
     * exists -- it's a compositor-specific protocol extension, not part
     * of core Wayland/GTK). */
    self->wl_display = gdk_wayland_display_get_wl_display(gdk_display);
    self->wl_registry = wl_display_get_registry(self->wl_display);
    wl_registry_add_listener(self->wl_registry, &registry_listener, self);
    wl_display_roundtrip(self->wl_display); /* block until globals are bound */

    if (self->data_control_manager) {
        GdkSeat *gdk_seat = gdk_display_get_default_seat(gdk_display);
        struct wl_seat *wl_seat = gdk_wayland_seat_get_wl_seat(gdk_seat);
        self->data_control_device =
            zwlr_data_control_manager_v1_get_data_device(self->data_control_manager, wl_seat);
        zwlr_data_control_device_v1_add_listener(self->data_control_device, &device_listener, self);
        syslog(LOG_DEBUG, "%s: bound zwlr_data_control_manager_v1=%p device=%p seat=%p",
               __func__, (void *)self->data_control_manager, (void *)self->data_control_device,
               (void *)wl_seat);
    } else {
        syslog(LOG_WARNING, "%s: compositor has no zwlr_data_control_manager_v1; "
                            "guest clipboard changes will not be observed", __func__);
    }
#endif

    return self;
}

void
vdagent_clipboards_set_conn(VDAgentClipboards *self, UdscsConnection *conn)
{
#ifdef USE_GTK_FOR_CLIPBOARD
    self->conn = conn;
#endif
}

static void vdagent_clipboards_dispose(GObject *obj)
{
#ifdef USE_GTK_FOR_CLIPBOARD
    VDAgentClipboards *self = VDAGENT_CLIPBOARDS(obj);

    if (self->conn)
        vdagent_clipboards_release_all(self);
#endif
}

static void
vdagent_clipboards_class_init(VDAgentClipboardsClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS(klass);

    oclass->dispose = vdagent_clipboards_dispose;
}
