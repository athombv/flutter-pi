// SPDX-License-Identifier: MIT
/*
 * WebRTC session
 *
 * Owns a GStreamer webrtcbin pipeline (offer/answer/ICE) for a single live
 * camera stream. Adapted from the retired libgst_webrtc_ffi.so with the FFI
 * registry removed and the offer SDP delivered via a callback (instead of a
 * blocking cond-wait), so the plugin layer can defer its platch reply until
 * the offer is actually ready. The decoded video pad is consumed by an
 * appsink and handed to the plugin layer via @ref webrtc_session_new_sample_cb,
 * which pushes the dmabuf NV12 sample onto a flutter-pi texture (zero-copy
 * EGLImage via gstreamer_video_player/frame.c).
 */

#ifndef _FLUTTERPI_SRC_PLUGINS_GSTREAMER_WEBRTC_SESSION_H
#define _FLUTTERPI_SRC_PLUGINS_GSTREAMER_WEBRTC_SESSION_H

#include <stdbool.h>

#include <gst/gst.h>

struct webrtc_session;

/// Invoked once, on the GLib main loop thread, when the SDP offer is ready
/// (ICE gathering complete or fallback timer expired). The caller must copy
/// @ref sdp if it needs to outlive this callback.
typedef void (*webrtc_session_offer_ready_cb)(struct webrtc_session *session, const char *sdp, void *userdata);

/// Invoked on the GLib main loop thread when the pipeline reports an
/// unrecoverable error (e.g. webrtcbin failed to create). Only setup failures
/// are reported back today; runtime bus errors are logged to stderr.
typedef void (*webrtc_session_error_cb)(struct webrtc_session *session, const char *message, void *userdata);

/// Invoked on the GStreamer streaming thread for each decoded video frame.
/// Ownership of @ref sample transfers to the callback — it MUST call
/// gst_sample_unref() (typically after handing the buffer to frame_new()).
typedef void (*webrtc_session_new_sample_cb)(struct webrtc_session *session, GstSample *sample, void *userdata);

/// Invoked on the GLib main loop thread once per session, the first time the
/// pipeline reports either `ice-connection-state` or `connection-state` as
/// FAILED. @ref reason is a short human-readable tag (e.g. "ice failed",
/// "peer connection failed") for diagnostics.
typedef void (*webrtc_session_connection_failed_cb)(struct webrtc_session *session, const char *reason, void *userdata);

/// Create a new WebRTC session and start building the pipeline asynchronously.
/// The callbacks fire on the internal GLib main loop thread (offer / error /
/// connection_failed) or the GStreamer streaming thread (new_sample). Returns
/// NULL on allocation failure.
struct webrtc_session *webrtc_session_new(
    bool enable_data_channel,
    webrtc_session_offer_ready_cb on_offer_ready,
    webrtc_session_error_cb on_error,
    webrtc_session_new_sample_cb on_new_sample,
    webrtc_session_connection_failed_cb on_connection_failed,
    void *userdata
);

/// Feed the SDP answer received from Homey/the broker.
/// Returns 0 on success, non-zero on validation/parse error.
int webrtc_session_set_answer(struct webrtc_session *session, const char *answer_sdp);

/// Tear down the pipeline and free the session. Safe to call from any thread.
void webrtc_session_destroy(struct webrtc_session *session);

#endif  // _FLUTTERPI_SRC_PLUGINS_GSTREAMER_WEBRTC_SESSION_H
