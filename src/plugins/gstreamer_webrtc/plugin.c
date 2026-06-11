// SPDX-License-Identifier: MIT
/*
 * GStreamer WebRTC plugin
 *
 * Native flutter-pi plugin exposing the `gstreamer_webrtc_player` method
 * channel. Runs WebRTC negotiation + decode in-process and pushes decoded
 * NV12 dmabuf samples onto a flutter-pi external texture (zero-copy EGLImage
 * via gstreamer_video_player/frame.c).
 *
 * createSession defers its method-channel reply until the real SDP offer is
 * ready (no polling). A videoSize event reports the source dimensions and a
 * firstFrame event fires once a frame is queued on the texture.
 */

#define _GNU_SOURCE

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

// gstreamer_video_player.h uses GstSample / GstVideoFormat without including
// the gst headers itself, so we have to pull them in first.
#include <gst/gst.h>
#include <gst/video/video.h>

#include "flutter-pi.h"
#include "platformchannel.h"
#include "pluginregistry.h"
#include "plugins/gstreamer_video_player.h"  // frame_interface, frame_new, frame_get_gl_frame, ...
#include "plugins/gstreamer_webrtc/session.h"
#include "texture_registry.h"
#include "util/logging.h"

#define WEBRTC_METHOD_CHANNEL "gstreamer_webrtc_player"
#define MAX_SESSIONS 8

// Always-on log (LOG_DEBUG is compiled out in release builds).
#define LOG(fmtstring, ...) fprintf(stderr, "[webrtc] " fmtstring "\n", ##__VA_ARGS__)

struct session_slot {
    bool in_use;
    int64_t session_id;
    struct texture *texture;
    int64_t texture_id;
    struct webrtc_session *gst_session;
    // The deferred createSession reply. NULL once we've responded.
    FlutterPlatformMessageResponseHandle *pending_create_handle;
    // Last source dimensions sent to Dart (so we only send `videoSize` when it
    // actually changes — usually just the first frame).
    int last_width;
    int last_height;
    // Whether we've notified Dart that the first frame is queued on the
    // texture. Used to gate the loading spinner on the widget side so the user
    // doesn't see a black `Texture` between status=streaming and first paint.
    bool first_frame_sent;
};

static struct {
    struct flutterpi *flutterpi;
    pthread_mutex_t mutex;
    int64_t next_session_id;
    struct session_slot sessions[MAX_SESSIONS];
} plugin = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .next_session_id = 1,
};

// Shared frame_interface (one per process is enough — it wraps EGL extension
// pointers + dmabuf format list for our GL renderer). Lazily created on the
// first createSession so the renderer is guaranteed to be up.
static gsize                  g_frame_interface_init = 0;
static struct frame_interface *g_frame_interface = NULL;

static int ensure_frame_interface(void) {
    if (g_once_init_enter(&g_frame_interface_init)) {
        struct gl_renderer *renderer = flutterpi_get_gl_renderer(plugin.flutterpi);
        if (renderer != NULL) {
            g_frame_interface = frame_interface_new(renderer);
            if (g_frame_interface == NULL) {
                LOG("frame_interface_new() failed — frames cannot be pushed to texture");
            }
        } else {
            LOG("flutterpi_get_gl_renderer() returned NULL — frames cannot be pushed to texture");
        }
        g_once_init_leave(&g_frame_interface_init, 1);
    }
    return g_frame_interface != NULL ? 0 : -1;
}

// Caller must hold plugin.mutex.
static struct session_slot *session_alloc_locked(void) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!plugin.sessions[i].in_use) return &plugin.sessions[i];
    }
    return NULL;
}

// Caller must hold plugin.mutex.
static struct session_slot *session_get_locked(int64_t id) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (plugin.sessions[i].in_use && plugin.sessions[i].session_id == id) {
            return &plugin.sessions[i];
        }
    }
    return NULL;
}

// Caller must hold plugin.mutex. Releases all slot resources except the
// gst_session, which is returned so the caller can destroy it outside the lock
// (webrtc_session_destroy is internally async, but we keep it off the lock to
// be safe against any future synchronous teardown).
static struct webrtc_session *session_release_locked(struct session_slot *s) {
    struct webrtc_session *gst = s->gst_session;
    if (s->texture != NULL) {
        texture_destroy(s->texture);
        s->texture = NULL;
    }
    s->gst_session = NULL;
    s->pending_create_handle = NULL;
    s->in_use = false;
    s->session_id = 0;
    s->texture_id = 0;
    s->last_width = 0;
    s->last_height = 0;
    s->first_frame_sent = false;
    return gst;
}

// ─── Session callbacks (invoked on the GLib main loop thread) ────────────────

static void on_session_offer_ready(struct webrtc_session *session, const char *sdp, void *userdata) {
    (void) session;
    int64_t id = (int64_t)(intptr_t) userdata;

    pthread_mutex_lock(&plugin.mutex);
    struct session_slot *s = session_get_locked(id);
    if (s == NULL || s->pending_create_handle == NULL) {
        // Disposed before the offer arrived, or already responded — drop it.
        pthread_mutex_unlock(&plugin.mutex);
        return;
    }
    FlutterPlatformMessageResponseHandle *handle = s->pending_create_handle;
    s->pending_create_handle = NULL;
    int64_t session_id = s->session_id;
    int64_t texture_id = s->texture_id;
    pthread_mutex_unlock(&plugin.mutex);

    LOG("createSession[%" PRId64 "]: offer ready (%zu bytes), responding to Dart", session_id, strlen(sdp));

    struct std_value result = STDMAP3(
        STDSTRING("sessionId"),
        STDINT64(session_id),
        STDSTRING("textureId"),
        STDINT64(texture_id),
        STDSTRING("offerSdp"),
        STDSTRING((char *) sdp)
    );
    platch_respond_success_std(handle, &result);
}

// texture_push_frame's destroy callback — releases the EGLImage/dmabuf-fd-backed
// video_frame once the engine is done compositing it.
static void on_destroy_texture_frame(const struct texture_frame *frame, void *userdata) {
    (void) frame;
    frame_destroy((struct video_frame *) userdata);
}

static void on_session_new_sample(struct webrtc_session *session, GstSample *sample, void *userdata) {
    (void) session;
    int64_t id = (int64_t)(intptr_t) userdata;

    // Extract source dimensions from the sample caps so the Texture widget
    // can use the right aspect ratio (otherwise it defaults to 480x480 and
    // squashes the video). Done before any locking so frame_new can run
    // without holding the plugin mutex.
    GstCaps *caps = gst_sample_get_caps(sample);
    GstVideoInfo vinfo;
    int sample_w = 0, sample_h = 0;
    if (caps != NULL && gst_video_info_from_caps(&vinfo, caps)) {
        sample_w = vinfo.width;
        sample_h = vinfo.height;
    }

    if (g_frame_interface == NULL) {
        // EGL/GL renderer wasn't ready when we tried to initialise the
        // frame_interface — drop the sample.
        gst_sample_unref(sample);
        return;
    }

    // EGLImage import + GL texture creation. Expensive and independent of the
    // plugin slot state, so we do it outside the mutex.
    struct video_frame *vf = frame_new(g_frame_interface, sample, NULL);
    gst_sample_unref(sample);
    if (vf == NULL) return;

    // Lock to: (a) update last_width/height under the slot, (b) push the
    // frame while we still hold a stable reference to the texture. If the
    // session was disposed in the meantime, the slot is gone and we destroy
    // the frame ourselves instead of leaking it.
    bool size_changed = false;
    bool first_frame = false;
    pthread_mutex_lock(&plugin.mutex);
    struct session_slot *s = session_get_locked(id);
    if (s != NULL && sample_w > 0 && sample_h > 0 &&
        (sample_w != s->last_width || sample_h != s->last_height)) {
        s->last_width = sample_w;
        s->last_height = sample_h;
        size_changed = true;
    }
    if (s != NULL && s->texture != NULL) {
        texture_push_frame(
            s->texture,
            &(struct texture_frame){
                .gl = *frame_get_gl_frame(vf),
                .destroy = on_destroy_texture_frame,
                .userdata = vf,
            }
        );
        if (!s->first_frame_sent) {
            s->first_frame_sent = true;
            first_frame = true;
        }
    } else {
        frame_destroy(vf);
    }
    pthread_mutex_unlock(&plugin.mutex);

    if (size_changed) {
        LOG("[%" PRId64 "] videoSize: %dx%d → Dart", id, sample_w, sample_h);
        struct std_value arg = STDMAP3(
            STDSTRING("sessionId"), STDINT64(id),
            STDSTRING("width"),     STDINT64(sample_w),
            STDSTRING("height"),    STDINT64(sample_h)
        );
        platch_call_std(WEBRTC_METHOD_CHANNEL, "videoSize", &arg, NULL, NULL);
    }
    if (first_frame) {
        LOG("[%" PRId64 "] firstFrame → Dart (texture has a queued frame)", id);
        struct std_value arg = STDMAP1(STDSTRING("sessionId"), STDINT64(id));
        platch_call_std(WEBRTC_METHOD_CHANNEL, "firstFrame", &arg, NULL, NULL);
    }
}

static void on_session_connection_failed(struct webrtc_session *session, const char *reason, void *userdata) {
    (void) session;
    int64_t id = (int64_t)(intptr_t) userdata;

    pthread_mutex_lock(&plugin.mutex);
    struct session_slot *s = session_get_locked(id);
    bool still_in_use = (s != NULL);
    pthread_mutex_unlock(&plugin.mutex);

    if (!still_in_use) return;  // already disposed

    LOG("[%" PRId64 "] connectionFailed: %s → Dart", id, reason);
    struct std_value arg = STDMAP2(
        STDSTRING("sessionId"), STDINT64(id),
        STDSTRING("reason"),    STDSTRING((char *) reason)
    );
    platch_call_std(WEBRTC_METHOD_CHANNEL, "connectionFailed", &arg, NULL, NULL);
}

static void on_session_error(struct webrtc_session *session, const char *message, void *userdata) {
    (void) session;
    int64_t id = (int64_t)(intptr_t) userdata;

    pthread_mutex_lock(&plugin.mutex);
    struct session_slot *s = session_get_locked(id);
    if (s == NULL || s->pending_create_handle == NULL) {
        pthread_mutex_unlock(&plugin.mutex);
        return;
    }
    FlutterPlatformMessageResponseHandle *handle = s->pending_create_handle;
    s->pending_create_handle = NULL;
    pthread_mutex_unlock(&plugin.mutex);

    LOG("createSession[%" PRId64 "]: session error: %s", id, message);
    platch_respond_error_std(handle, "session-error", (char *) message, NULL);
}

// ─── Method handlers ─────────────────────────────────────────────────────────

static int on_create_session(struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    bool enable_data_channel = false;
    struct std_value *dc = stdmap_get_str(&object->std_arg, "dataChannel");
    if (dc != NULL && STDVALUE_IS_BOOL(*dc)) {
        enable_data_channel = STDVALUE_AS_BOOL(*dc);
    }

    struct texture_registry *reg = flutterpi_get_texture_registry(plugin.flutterpi);
    if (reg == NULL) {
        return platch_respond_error_std(responsehandle, "no-texture-registry", "flutter-pi texture registry unavailable", NULL);
    }
    if (ensure_frame_interface() != 0) {
        return platch_respond_error_std(responsehandle, "no-gl-renderer", "GL renderer / frame_interface unavailable", NULL);
    }

    pthread_mutex_lock(&plugin.mutex);

    struct session_slot *slot = session_alloc_locked();
    if (slot == NULL) {
        pthread_mutex_unlock(&plugin.mutex);
        return platch_respond_error_std(responsehandle, "too-many-sessions", "no free WebRTC session slot", NULL);
    }

    struct texture *texture = texture_new(reg);
    if (texture == NULL) {
        pthread_mutex_unlock(&plugin.mutex);
        return platch_respond_error_std(responsehandle, "texture-failed", "texture_new() failed", NULL);
    }

    slot->in_use = true;
    slot->session_id = plugin.next_session_id++;
    slot->texture = texture;
    slot->texture_id = texture_get_id(texture);
    slot->pending_create_handle = responsehandle;

    int64_t session_id = slot->session_id;
    int64_t texture_id = slot->texture_id;

    // Kick off the WebRTC pipeline. The offer SDP comes back via the callback
    // on the GLib thread; this thread does not block.
    struct webrtc_session *gst = webrtc_session_new(
        enable_data_channel,
        on_session_offer_ready,
        on_session_error,
        on_session_new_sample,
        on_session_connection_failed,
        (void *)(intptr_t) session_id
    );
    if (gst == NULL) {
        slot->pending_create_handle = NULL;
        session_release_locked(slot);
        pthread_mutex_unlock(&plugin.mutex);
        return platch_respond_error_std(responsehandle, "session-failed", "webrtc_session_new() failed", NULL);
    }
    slot->gst_session = gst;

    pthread_mutex_unlock(&plugin.mutex);

    LOG("createSession[%" PRId64 "]: texture=%" PRId64 " dataChannel=%s — awaiting offer",
        session_id, texture_id, enable_data_channel ? "true" : "false");

    // No platch response here — we'll reply from on_session_offer_ready.
    return 0;
}

static int on_set_answer(struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct std_value *id_v = stdmap_get_str(&object->std_arg, "sessionId");
    struct std_value *sdp_v = stdmap_get_str(&object->std_arg, "answerSdp");
    if (id_v == NULL || !STDVALUE_IS_INT(*id_v)) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['sessionId']` to be an integer.");
    }
    if (sdp_v == NULL || sdp_v->type != kStdString) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['answerSdp']` to be a string.");
    }

    pthread_mutex_lock(&plugin.mutex);
    struct session_slot *slot = session_get_locked(STDVALUE_AS_INT(*id_v));
    struct webrtc_session *gst = slot ? slot->gst_session : NULL;
    int64_t session_id = slot ? slot->session_id : 0;
    pthread_mutex_unlock(&plugin.mutex);

    if (gst == NULL) {
        return platch_respond_error_std(responsehandle, "no-session", "unknown sessionId", NULL);
    }

    int rc = webrtc_session_set_answer(gst, sdp_v->string_value);
    if (rc != 0) {
        LOG("setAnswer[%" PRId64 "]: rejected by session (rc=%d)", session_id, rc);
        return platch_respond_error_std(responsehandle, "set-answer-failed", "WebRTC set-answer rejected", NULL);
    }
    LOG("setAnswer[%" PRId64 "]: submitted", session_id);
    return platch_respond_success_std(responsehandle, NULL);
}

static int on_dispose_session(struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct std_value *id_v = stdmap_get_str(&object->std_arg, "sessionId");
    if (id_v == NULL || !STDVALUE_IS_INT(*id_v)) {
        return platch_respond_illegal_arg_std(responsehandle, "Expected `arg['sessionId']` to be an integer.");
    }
    int64_t id = STDVALUE_AS_INT(*id_v);

    pthread_mutex_lock(&plugin.mutex);
    struct session_slot *slot = session_get_locked(id);
    struct webrtc_session *gst = NULL;
    FlutterPlatformMessageResponseHandle *pending = NULL;
    if (slot != NULL) {
        pending = slot->pending_create_handle;
        slot->pending_create_handle = NULL;
        gst = session_release_locked(slot);
        LOG("disposeSession[%" PRId64 "]: slot released", id);
    }
    pthread_mutex_unlock(&plugin.mutex);

    // If createSession hadn't replied yet, fail it cleanly before the pipeline
    // teardown so Dart isn't left awaiting forever.
    if (pending != NULL) {
        platch_respond_error_std(pending, "session-disposed", "session disposed before offer ready", NULL);
    }
    if (gst != NULL) {
        webrtc_session_destroy(gst);
    }
    return platch_respond_success_std(responsehandle, NULL);
}

static int on_method_call(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    (void) channel;
    const char *method = object->method;

    if (strcmp(method, "createSession") == 0) {
        return on_create_session(object, responsehandle);
    } else if (strcmp(method, "setAnswer") == 0) {
        return on_set_answer(object, responsehandle);
    } else if (strcmp(method, "disposeSession") == 0) {
        return on_dispose_session(object, responsehandle);
    }

    LOG_DEBUG("[webrtc] unimplemented method: %s\n", method);
    return platch_respond_not_implemented(responsehandle);
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

enum plugin_init_result gstreamer_webrtc_plugin_init(struct flutterpi *flutterpi, void **userdata_out) {
    (void) userdata_out;

    plugin.flutterpi = flutterpi;

    int ok = plugin_registry_set_receiver_locked(WEBRTC_METHOD_CHANNEL, kStandardMethodCall, on_method_call);
    if (ok != 0) {
        LOG_ERROR("[webrtc] could not register method channel receiver: %s\n", strerror(ok));
        return PLUGIN_INIT_RESULT_ERROR;
    }

    LOG("plugin initialized: webrtcbin + DMA-BUF texture push");
    return PLUGIN_INIT_RESULT_INITIALIZED;
}

void gstreamer_webrtc_plugin_deinit(struct flutterpi *flutterpi, void *userdata) {
    (void) flutterpi;
    (void) userdata;

    plugin_registry_remove_receiver(WEBRTC_METHOD_CHANNEL);

    pthread_mutex_lock(&plugin.mutex);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (plugin.sessions[i].in_use) {
            struct webrtc_session *gst = session_release_locked(&plugin.sessions[i]);
            if (gst != NULL) webrtc_session_destroy(gst);
        }
    }
    pthread_mutex_unlock(&plugin.mutex);
}

FLUTTERPI_PLUGIN("gstreamer webrtc", gstreamer_webrtc, gstreamer_webrtc_plugin_init, gstreamer_webrtc_plugin_deinit)
