// SPDX-License-Identifier: MIT
/*
 * WebRTC session
 *
 * Owns a webrtcbin pipeline (negotiation + decode). Offer SDP is delivered
 * via callback so the plugin layer can use a deferred platch reply. Decoded
 * NV12 samples leave at an appsink; the plugin layer hands them to
 * gstreamer_video_player/frame.c which pushes DMA-BUF → EGLImage → texture
 * (true zero-copy).
 */

#define _GNU_SOURCE

#include "plugins/gstreamer_webrtc/session.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GST_USE_UNSTABLE_API  // ack: webrtcbin is in gst-plugins-bad

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <gst/video/video.h>

#define LOG(fmtstring, ...) fprintf(stderr, "[webrtc] " fmtstring "\n", ##__VA_ARGS__)

#define ICE_GATHERING_FALLBACK_MS 5000

// ─── Internal session ─────────────────────────────────────────────────────────

struct webrtc_session {
    int64_t     id;
    gboolean    enable_data_channel;

    GstElement *pipeline;
    GstElement *webrtcbin;

    gboolean    offer_signalled;   // guarded by s_glib_loop (we only touch on GLib thread)
    guint       ice_timeout_source;
    guint       pli_timer_source;

    gboolean    has_keyframe;
    gboolean    rtp_probe_logged;
    gboolean    video_pad_added;
    gboolean    audio_pad_added;
    guint       pli_sent_count;

    // Decoded-sample FPS, logged every 3s.
    gint64      fps_window_start_us;
    gint64      fps_sample_count;

    webrtc_session_offer_ready_cb       offer_ready_cb;
    webrtc_session_error_cb             error_cb;
    webrtc_session_new_sample_cb        new_sample_cb;
    webrtc_session_connection_failed_cb connection_failed_cb;
    void                               *userdata;

    // True once we've notified the caller of an ICE/connection failure.
    // Both `ice-connection-state → FAILED` and `connection-state → FAILED`
    // can fire (and can fire repeatedly); we only forward the first one.
    gboolean    failure_signalled;
};

// Forward decl: on_connection_state_change starts the recurring PLI timer
// before send_pli_cb is defined further down the file.
static gboolean send_pli_cb(gpointer data);

// ─── appsink callback: hand decoded samples to the plugin layer ──────────────

static GstFlowReturn on_appsink_new_sample(GstAppSink *appsink, gpointer data) {
    struct webrtc_session *s = data;
    GstSample *sample = gst_app_sink_try_pull_sample(appsink, 0);
    if (sample == NULL) return GST_FLOW_OK;

    // First decoded frame seen — stop nagging with PLI.
    if (!s->has_keyframe) {
        s->has_keyframe = TRUE;
        LOG("[%" G_GINT64_FORMAT "] first decoded sample received", s->id);
    }

    // FPS log every 3s (decoded samples / elapsed).
    gint64 now_us = g_get_monotonic_time();
    if (s->fps_window_start_us == 0) s->fps_window_start_us = now_us;
    s->fps_sample_count++;
    gint64 window_us = now_us - s->fps_window_start_us;
    if (window_us >= 3000000) {
        LOG("[%" G_GINT64_FORMAT "] FPS: %.1f  (%" G_GINT64_FORMAT " samples over %.1fs)",
            s->id,
            s->fps_sample_count * 1e6 / window_us,
            s->fps_sample_count,
            window_us / 1e6);
        s->fps_window_start_us = now_us;
        s->fps_sample_count = 0;
    }

    if (s->new_sample_cb != NULL) {
        // Ownership passes to the callback (it must gst_sample_unref).
        s->new_sample_cb(s, sample, s->userdata);
    } else {
        gst_sample_unref(sample);
    }
    return GST_FLOW_OK;
}

static GMainLoop *s_glib_loop   = NULL;
static GThread   *s_glib_thread = NULL;
static gboolean   s_gst_inited  = FALSE;
static int64_t    s_next_id     = 1;

// ─── GLib main loop (flutter-pi uses sd-event, not GLib — run our own) ────────

static gpointer glib_main_loop_thread(gpointer data) {
    (void) data;
    s_glib_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(s_glib_loop);
    return NULL;
}

static void ensure_glib_running(void) {
    static gsize initialised = 0;
    if (g_once_init_enter(&initialised)) {
        if (!s_gst_inited) {
            gst_init(NULL, NULL);
            s_gst_inited = TRUE;
            LOG("GStreamer initialised, version %s", gst_version_string());
        }
        s_glib_thread = g_thread_new("gst-webrtc-glib", glib_main_loop_thread, NULL);
        g_once_init_leave(&initialised, 1);
    }
}

// ─── SDP patching (same as FFI, needed for Nest compatibility) ────────────────

static gchar *patch_video_sdp(const gchar *sdp) {
    GString *patched = g_string_new("");
    const gchar *line = sdp;
    gboolean in_audio_mline = FALSE;
    gboolean in_video_mline = FALSE;
    gboolean in_application_mline = FALSE;
    gboolean audio_fmtp_added = FALSE;

    while (line && *line) {
        const gchar *next_line = strchr(line, '\n');
        size_t line_len = next_line ? (size_t)(next_line - line) : strlen(line);
        gchar *line_copy = g_strndup(line, line_len);
        gsize copy_len = strlen(line_copy);
        if (copy_len > 0 && line_copy[copy_len - 1] == '\r') line_copy[copy_len - 1] = '\0';

        if (g_str_has_prefix(line_copy, "m=")) {
            in_audio_mline = g_str_has_prefix(line_copy, "m=audio");
            in_video_mline = g_str_has_prefix(line_copy, "m=video");
            in_application_mline = g_str_has_prefix(line_copy, "m=application");
            audio_fmtp_added = FALSE;
        }

        if (g_str_has_prefix(line_copy, "a=rtcp-mux-only")) {
            g_free(line_copy);
            line = next_line ? (next_line + 1) : NULL;
            continue;
        }

        if (g_str_has_prefix(line_copy, "m=video 0 ")) {
            gchar *fixed = g_strdup_printf("m=video 9 %s", line_copy + strlen("m=video 0 "));
            g_string_append(patched, fixed);
            g_string_append_c(patched, '\n');
            g_free(fixed);
        } else if (g_str_has_prefix(line_copy, "m=application 0 ")) {
            gchar *fixed = g_strdup_printf("m=application 9 %s", line_copy + strlen("m=application 0 "));
            g_string_append(patched, fixed);
            g_string_append_c(patched, '\n');
            g_free(fixed);
        } else if (in_audio_mline && g_str_has_prefix(line_copy, "a=rtpmap:97")) {
            gchar *opus_pos = strstr(line_copy, "OPUS/");
            if (opus_pos) {
                opus_pos[0] = 'o'; opus_pos[1] = 'p'; opus_pos[2] = 'u'; opus_pos[3] = 's';
            }
            if (!strstr(line_copy, "/48000/")) {
                gchar *fixed = g_strdup_printf("%s/2", line_copy);
                g_string_append(patched, fixed);
                g_string_append_c(patched, '\n');
                g_free(fixed);
            } else {
                g_string_append(patched, line_copy);
                g_string_append_c(patched, '\n');
            }
        } else if (in_application_mline && g_str_has_prefix(line_copy, "a=sctp-port:5000")) {
            g_string_append(patched, "a=sctpmap:5000 webrtc-datachannel 1024\n");
        } else if (g_str_has_prefix(line_copy, "a=group:BUNDLE ")) {
            // Nest does not echo offerer MID values; it always uses 0/1/2.
            g_string_append(patched, "a=group:BUNDLE 0 1 2\n");
        } else if (in_audio_mline && g_str_has_prefix(line_copy, "a=mid:")) {
            g_string_append(patched, "a=mid:0\n");
        } else if (in_video_mline && g_str_has_prefix(line_copy, "a=mid:")) {
            g_string_append(patched, "a=mid:1\n");
        } else if (in_application_mline && g_str_has_prefix(line_copy, "a=mid:")) {
            g_string_append(patched, "a=mid:2\n");
        } else {
            g_string_append(patched, line_copy);
            g_string_append_c(patched, '\n');
        }

        if (in_audio_mline && !audio_fmtp_added && g_str_has_prefix(line_copy, "a=rtpmap:97")) {
            g_string_append(patched, "a=fmtp:97 minptime=10;useinbandfec=1\n");
            audio_fmtp_added = TRUE;
        }
        if (in_video_mline && g_str_has_prefix(line_copy, "a=rtpmap:103 H264")) {
            g_string_append(patched, "a=fmtp:103 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\n");
        }

        g_free(line_copy);
        line = next_line ? (next_line + 1) : NULL;
    }

    return g_string_free(patched, FALSE);
}

// ─── Offer signalling ─────────────────────────────────────────────────────────

static void signal_offer(struct webrtc_session *s) {
    if (s->offer_signalled) return;

    GstWebRTCSessionDescription *local_desc = NULL;
    g_object_get(s->webrtcbin, "local-description", &local_desc, NULL);
    if (!local_desc) {
        LOG("[%" G_GINT64_FORMAT "] signal_offer: failed to get local-description", s->id);
        return;
    }
    gchar *raw_sdp = gst_sdp_message_as_text(local_desc->sdp);
    gst_webrtc_session_description_free(local_desc);

    gchar *sdp_str = patch_video_sdp(raw_sdp);
    g_free(raw_sdp);

    s->offer_signalled = TRUE;
    LOG("[%" G_GINT64_FORMAT "] signal_offer: offer ready (%zu bytes)", s->id, strlen(sdp_str));

    if (s->offer_ready_cb) {
        s->offer_ready_cb(s, sdp_str, s->userdata);
    }
    g_free(sdp_str);
}

static gboolean ice_gathering_timeout(gpointer data) {
    struct webrtc_session *s = data;
    s->ice_timeout_source = 0;
    LOG("[%" G_GINT64_FORMAT "] ice_gathering_timeout: %dms elapsed, signalling offer",
        s->id, ICE_GATHERING_FALLBACK_MS);
    signal_offer(s);
    return G_SOURCE_REMOVE;
}

static void on_offer_created(GstPromise *promise, gpointer data) {
    struct webrtc_session *s = data;

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = NULL;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    if (!offer) {
        LOG("[%" G_GINT64_FORMAT "] on_offer_created: no offer in reply", s->id);
        return;
    }

    // Set local description — ICE gathering starts now. We wait for gathering
    // to complete so the offer SDP includes local candidates.
    GstPromise *local = gst_promise_new();
    g_signal_emit_by_name(s->webrtcbin, "set-local-description", offer, local);
    gst_promise_interrupt(local);
    gst_promise_unref(local);
    gst_webrtc_session_description_free(offer);

    s->ice_timeout_source = g_timeout_add(ICE_GATHERING_FALLBACK_MS, ice_gathering_timeout, s);
}

static void on_ice_candidate(GstElement *webrtcbin, guint mline_index, gchar *candidate, gpointer data) {
    (void) webrtcbin;
    (void) mline_index;
    struct webrtc_session *s = data;
    if (candidate == NULL || candidate[0] == '\0') {
        if (s->ice_timeout_source != 0) {
            g_source_remove(s->ice_timeout_source);
            s->ice_timeout_source = 0;
        }
        signal_offer(s);
    }
}

static void on_ice_gathering_state_change(GstElement *webrtcbin, GParamSpec *pspec, gpointer data) {
    (void) pspec;
    struct webrtc_session *s = data;
    GstWebRTCICEGatheringState state;
    g_object_get(webrtcbin, "ice-gathering-state", &state, NULL);

    const gchar *name = "?";
    switch (state) {
        case GST_WEBRTC_ICE_GATHERING_STATE_NEW:       name = "new";       break;
        case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING: name = "gathering"; break;
        case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:  name = "complete";  break;
    }
    LOG("[%" G_GINT64_FORMAT "] ice-gathering-state → %s", s->id, name);

    if (state == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) {
        LOG("[%" G_GINT64_FORMAT "] ice offer ready", s->id);
        if (s->ice_timeout_source != 0) {
            g_source_remove(s->ice_timeout_source);
            s->ice_timeout_source = 0;
        }
        signal_offer(s);
    }
}

static void on_connection_state_change(GstElement *webrtcbin, GParamSpec *pspec, gpointer data) {
    (void) pspec;
    struct webrtc_session *s = data;
    GstWebRTCPeerConnectionState state;
    g_object_get(webrtcbin, "connection-state", &state, NULL);

    const gchar *name = "?";
    switch (state) {
        case GST_WEBRTC_PEER_CONNECTION_STATE_NEW:         name = "new";          break;
        case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING:  name = "connecting";   break;
        case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED:   name = "connected";    break;
        case GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED:name = "disconnected"; break;
        case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED:      name = "failed";       break;
        case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED:      name = "closed";       break;
    }
    LOG("[%" G_GINT64_FORMAT "] connection-state → %s", s->id, name);

    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED) {
        GstEvent *fku = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
        gst_element_send_event(s->webrtcbin, fku);
        s->pli_sent_count++;
        // Start the recurring PLI timer NOW (not just on pad-added) so we
        // keep nagging the camera every 500ms during the wait for the first
        // keyframe — important for cloud-relayed cameras (Nest) that can
        // take many seconds to start sending video. 500ms (vs 2s previously)
        // closes the typical multi-second gap between camera-becomes-ready
        // and our next nag landing.
        if (s->pli_timer_source == 0) {
            s->pli_timer_source = g_timeout_add(500, send_pli_cb, s);
        }
    } else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED) {
        if (s->connection_failed_cb != NULL && !s->failure_signalled) {
            s->failure_signalled = TRUE;
            s->connection_failed_cb(s, "peer connection failed", s->userdata);
        }
    }
}

static void on_ice_connection_state_change(GstElement *webrtcbin, GParamSpec *pspec, gpointer data) {
    (void) pspec;
    struct webrtc_session *s = data;
    GstWebRTCICEConnectionState state;
    g_object_get(webrtcbin, "ice-connection-state", &state, NULL);

    const gchar *name = "?";
    switch (state) {
        case GST_WEBRTC_ICE_CONNECTION_STATE_NEW:          name = "new";          break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING:     name = "checking";     break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:    name = "connected";    break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED:    name = "completed";    break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:       name = "failed";       break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED: name = "disconnected"; break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED:       name = "closed";       break;
    }
    LOG("[%" G_GINT64_FORMAT "] ice-connection-state → %s", s->id, name);

    if (state == GST_WEBRTC_ICE_CONNECTION_STATE_FAILED) {
        if (s->connection_failed_cb != NULL && !s->failure_signalled) {
            s->failure_signalled = TRUE;
            s->connection_failed_cb(s, "ice failed", s->userdata);
        }
    }
}

// Periodic PLI requests until the first keyframe arrives (Nest needs nagging).
static gboolean send_pli_cb(gpointer data) {
    struct webrtc_session *s = data;
    if (s->has_keyframe) {
        s->pli_timer_source = 0;
        return G_SOURCE_REMOVE;
    }
    GstEvent *fku = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
    gst_element_send_event(s->webrtcbin, fku);
    s->pli_sent_count++;
    return G_SOURCE_CONTINUE;
}

static GstPadProbeReturn on_rtp_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    (void) pad;
    (void) info;
    struct webrtc_session *s = data;
    if (!s->rtp_probe_logged) {
        s->rtp_probe_logged = TRUE;
        LOG("[%" G_GINT64_FORMAT "] rtp: first packet received on incoming video pad", s->id);
    }
    return GST_PAD_PROBE_OK;
}

// ─── pad-added: build decode chain → appsink ─────────────────────────────────

static void on_pad_added(GstElement *webrtcbin, GstPad *new_pad, gpointer data) {
    (void) webrtcbin;
    struct webrtc_session *s = data;

    GstCaps *caps = gst_pad_get_current_caps(new_pad);
    if (!caps) caps = gst_pad_query_caps(new_pad, NULL);
    gchar *caps_str = caps ? gst_caps_to_string(caps) : g_strdup("(unknown)");

    gboolean is_video = caps && !gst_caps_is_empty(caps) && g_strstr_len(caps_str, -1, "video") != NULL;
    gboolean is_audio = caps && !gst_caps_is_empty(caps) && g_strstr_len(caps_str, -1, "audio") != NULL;

    const gchar *encoding_name = NULL;
    gint payload_type = -1;
    gchar *encoding_upper = NULL;
    if (caps && gst_caps_get_size(caps) > 0) {
        GstStructure *st = gst_caps_get_structure(caps, 0);
        const gchar *enc = gst_structure_get_string(st, "encoding-name");
        if (enc) {
            encoding_upper = g_ascii_strup(enc, -1);
            encoding_name = encoding_upper;
        }
        gst_structure_get_int(st, "payload", &payload_type);
    }

    LOG("[%" G_GINT64_FORMAT "] pad-added: caps=%s encoding=%s payload=%d",
        s->id, caps_str, encoding_name ? encoding_name : "(none)", payload_type);

    g_free(caps_str);
    if (caps) gst_caps_unref(caps);

    if (is_audio) {
        s->audio_pad_added = TRUE;
        GstElement *fakesink = gst_element_factory_make("fakesink", NULL);
        if (fakesink) {
            g_object_set(fakesink, "sync", FALSE, "async", FALSE, NULL);
            gst_bin_add(GST_BIN(s->pipeline), fakesink);
            GstPad *sink = gst_element_get_static_pad(fakesink, "sink");
            gst_pad_link(new_pad, sink);
            gst_object_unref(sink);
            gst_element_sync_state_with_parent(fakesink);
        }
        g_free(encoding_upper);
        return;
    }

    if (!is_video) {
        LOG("[%" G_GINT64_FORMAT "] pad-added: unknown pad type, ignoring", s->id);
        g_free(encoding_upper);
        return;
    }

    s->video_pad_added = TRUE;

    GstElement *queue   = gst_element_factory_make("queue",   NULL);
    GstElement *depay   = NULL;
    GstElement *parse   = NULL;
    GstElement *decode  = NULL;
    GstElement *sink    = gst_element_factory_make("appsink", NULL);
    gboolean    uses_parse = FALSE;

    if (encoding_name && g_strcmp0(encoding_name, "H264") == 0) {
        depay = gst_element_factory_make("rtph264depay", NULL);
        parse = gst_element_factory_make("h264parse", NULL); uses_parse = TRUE;
        decode = gst_element_factory_make("mppvideodec", NULL);
        if (decode) g_object_set(decode, "arm-afbc", FALSE, NULL);
        else        decode = gst_element_factory_make("avdec_h264", NULL);
    } else if (encoding_name && g_strcmp0(encoding_name, "H265") == 0) {
        depay = gst_element_factory_make("rtph265depay", NULL);
        parse = gst_element_factory_make("h265parse", NULL); uses_parse = TRUE;
        decode = gst_element_factory_make("mppvideodec", NULL);
        if (decode) g_object_set(decode, "arm-afbc", FALSE, NULL);
        else        decode = gst_element_factory_make("avdec_h265", NULL);
    } else if (encoding_name && g_strcmp0(encoding_name, "VP8") == 0) {
        depay = gst_element_factory_make("rtpvp8depay", NULL);
        decode = gst_element_factory_make("vp8dec", NULL);
    } else if (encoding_name && g_strcmp0(encoding_name, "VP9") == 0) {
        depay = gst_element_factory_make("rtpvp9depay", NULL);
        decode = gst_element_factory_make("vp9dec", NULL);
    } else if (payload_type == 96) {
        // Generic VP8 by payload number (no encoding-name in caps).
        depay = gst_element_factory_make("rtpvp8depay", NULL);
        decode = gst_element_factory_make("vp8dec", NULL);
    } else if (payload_type == 98) {
        // Generic VP9 by payload number.
        depay = gst_element_factory_make("rtpvp9depay", NULL);
        decode = gst_element_factory_make("vp9dec", NULL);
    } else if (payload_type == 103) {
        depay = gst_element_factory_make("rtph264depay", NULL);
        parse = gst_element_factory_make("h264parse", NULL); uses_parse = TRUE;
        decode = gst_element_factory_make("mppvideodec", NULL);
        if (decode) g_object_set(decode, "arm-afbc", FALSE, NULL);
        else        decode = gst_element_factory_make("avdec_h264", NULL);
    } else if (payload_type == 104) {
        // Generic H265 by payload number (matches our codec preference).
        depay = gst_element_factory_make("rtph265depay", NULL);
        parse = gst_element_factory_make("h265parse", NULL); uses_parse = TRUE;
        decode = gst_element_factory_make("mppvideodec", NULL);
        if (decode) g_object_set(decode, "arm-afbc", FALSE, NULL);
        else        decode = gst_element_factory_make("avdec_h265", NULL);
    } else {
        LOG("[%" G_GINT64_FORMAT "] pad-added: no codec match (encoding=%s payload=%d), trying VP8 fallback",
            s->id, encoding_name ? encoding_name : "(none)", payload_type);
        depay = gst_element_factory_make("rtpvp8depay", NULL);
        decode = gst_element_factory_make("vp8dec", NULL);
    }

    if (!queue || !depay || !decode || !sink || (uses_parse && !parse)) {
        LOG("[%" G_GINT64_FORMAT "] pad-added: failed to create decode chain elements", s->id);
        g_free(encoding_upper);
        return;
    }

    g_object_set(queue, "max-size-buffers", 500, NULL);
    if (uses_parse) g_object_set(parse, "config-interval", -1, NULL);
    // appsink: deliver every fresh sample to our new_sample callback; drop on
    // backpressure (we render at display rate, not decode rate).
    g_object_set(sink, "emit-signals", FALSE, "sync", FALSE, "max-buffers", 1, "drop", TRUE, NULL);
    GstAppSinkCallbacks cbs = { .eos = NULL, .new_preroll = NULL, .new_sample = on_appsink_new_sample };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs, s, NULL);

    if (uses_parse) {
        gst_bin_add_many(GST_BIN(s->pipeline), queue, depay, parse, decode, sink, NULL);
        if (!gst_element_link_many(queue, depay, parse, decode, sink, NULL)) {
            LOG("[%" G_GINT64_FORMAT "] pad-added: failed to link decode chain (with parse)", s->id);
            g_free(encoding_upper);
            return;
        }
    } else {
        gst_bin_add_many(GST_BIN(s->pipeline), queue, depay, decode, sink, NULL);
        if (!gst_element_link_many(queue, depay, decode, sink, NULL)) {
            LOG("[%" G_GINT64_FORMAT "] pad-added: failed to link decode chain", s->id);
            g_free(encoding_upper);
            return;
        }
    }

    GstPad *queue_sink = gst_element_get_static_pad(queue, "sink");
    gst_pad_add_probe(new_pad, GST_PAD_PROBE_TYPE_BUFFER, (GstPadProbeCallback) on_rtp_probe, s, NULL);
    GstPadLinkReturn link_ret = gst_pad_link(new_pad, queue_sink);
    gst_object_unref(queue_sink);
    if (link_ret != GST_PAD_LINK_OK) {
        LOG("[%" G_GINT64_FORMAT "] pad-added: pad link failed: %d", s->id, link_ret);
        g_free(encoding_upper);
        return;
    }

    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(depay);
    if (uses_parse) gst_element_sync_state_with_parent(parse);
    gst_element_sync_state_with_parent(decode);
    gst_element_sync_state_with_parent(sink);
    LOG("[%" G_GINT64_FORMAT "] pad-added: video decode chain linked → appsink", s->id);

    // Kick a PLI immediately + start a 500ms recurring PLI until first keyframe.
    if (s->pli_timer_source == 0) {
        GstEvent *fku = gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0);
        gst_element_send_event(s->webrtcbin, fku);
        s->pli_sent_count++;
        s->pli_timer_source = g_timeout_add(500, send_pli_cb, s);
    }

    g_free(encoding_upper);
}

// ─── Bus messages ─────────────────────────────────────────────────────────────

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer data) {
    (void) bus;
    struct webrtc_session *s = data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar  *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            LOG("[%" G_GINT64_FORMAT "] BUS ERROR from %s: %s | %s",
                s->id, GST_OBJECT_NAME(msg->src), err->message, dbg ? dbg : "");
            // DTLS handshake failures are common when the remote rejects our
            // cert/transport — dump enough state to diagnose without re-reading
            // the whole log.
            const gchar *src_name = GST_OBJECT_NAME(msg->src);
            if ((src_name && g_str_has_prefix(src_name, "dtlsdec")) ||
                (dbg && g_strstr_len(dbg, -1, "gstdtlsdec.c"))) {
                LOG("[%" G_GINT64_FORMAT "] DTLS diagnostics: video_pad_added=%d audio_pad_added=%d "
                    "rtp_seen=%d has_keyframe=%d pli_sent=%u",
                    s->id, s->video_pad_added, s->audio_pad_added,
                    s->rtp_probe_logged, s->has_keyframe, s->pli_sent_count);
            }
            g_error_free(err);
            g_free(dbg);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err = NULL;
            gchar  *dbg = NULL;
            gst_message_parse_warning(msg, &err, &dbg);
            LOG("[%" G_GINT64_FORMAT "] BUS WARNING from %s: %s | %s",
                s->id, GST_OBJECT_NAME(msg->src), err->message, dbg ? dbg : "");
            g_error_free(err);
            g_free(dbg);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

// ─── Pipeline setup (runs on GLib main loop) ──────────────────────────────────

static gboolean do_session_setup(gpointer data) {
    struct webrtc_session *s = data;
    LOG("[%" G_GINT64_FORMAT "] do_session_setup: running on GLib main loop", s->id);

    s->pipeline  = gst_pipeline_new("webrtc-pipeline");
    s->webrtcbin = gst_element_factory_make("webrtcbin", "webrtc");
    if (!s->pipeline || !s->webrtcbin) {
        LOG("[%" G_GINT64_FORMAT "] do_session_setup: failed to create pipeline or webrtcbin", s->id);
        if (s->error_cb) s->error_cb(s, "failed to create webrtcbin", s->userdata);
        return G_SOURCE_REMOVE;
    }

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(s->pipeline));
    gst_bus_add_watch(bus, on_bus_message, s);
    gst_object_unref(bus);

    g_object_set(s->webrtcbin,
        "bundle-policy", 3 /* max-bundle */,
        "stun-server", "stun://stun.l.google.com:19302",
        "latency", 100,
        NULL);
    gst_bin_add(GST_BIN(s->pipeline), s->webrtcbin);

    // Disable TCP ICE candidates: on a LAN with UDP open they're pure gathering
    // tax (~10 of 15 candidates, ~1.5s stalled on sockets that never get selected).
    // `ice-tcp` isn't a webrtcbin property in gstreamer 1.22 — it lives on the
    // underlying GstWebRTCNice agent, exposed via webrtcbin's `ice-agent` property.
    {
        GObject *ice_agent = NULL;
        g_object_get(s->webrtcbin, "ice-agent", &ice_agent, NULL);
        if (ice_agent != NULL) {
            g_object_set(ice_agent, "ice-tcp", FALSE, NULL);
            g_object_unref(ice_agent);
            LOG("[%" G_GINT64_FORMAT "] ice-agent: ice-tcp disabled", s->id);
        } else {
            LOG("[%" G_GINT64_FORMAT "] ice-agent unavailable, cannot disable TCP gathering", s->id);
        }
    }

    GstElement *rtpbin = gst_bin_get_by_name(GST_BIN(s->webrtcbin), "rtpbin");
    if (rtpbin) {
        g_object_set(rtpbin, "do-retransmission", TRUE, NULL);
        gst_object_unref(rtpbin);
    }

    // Audio transceiver (required by most peers including Nest)
    GstCaps *audio_caps = gst_caps_from_string(
        "application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000,channels=2");
    GstWebRTCRTPTransceiver *audio_trans = NULL;
    g_signal_emit_by_name(s->webrtcbin, "add-transceiver",
        GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, audio_caps, &audio_trans);
    if (audio_trans) gst_object_unref(audio_trans);
    gst_caps_unref(audio_caps);

    // Video transceiver + codec preferences (VP8/VP9/H264/H265)
    //
    // H265 is last: it only wins when the sender has nothing else, which is
    // exactly the H265-only camera case (go2rtc cannot transcode, so an
    // H265 source otherwise fails negotiation with "codecs not matched").
    // No a=fmtp is emitted for it — go2rtc matches H265 on name + clock-rate
    // only, and an unmatched profile/level would turn a working stream into
    // a rejected one.
    GstCaps *video_caps = gst_caps_from_string(
        "application/x-rtp,media=video,encoding-name=H264,payload=103,clock-rate=90000");
    GstCaps *video_pref_caps = gst_caps_from_string(
        "application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000;"
        "application/x-rtp,media=video,encoding-name=VP9,payload=98,clock-rate=90000;"
        "application/x-rtp,media=video,encoding-name=H264,payload=103,clock-rate=90000;"
        "application/x-rtp,media=video,encoding-name=H265,payload=104,clock-rate=90000");
    GstWebRTCRTPTransceiver *video_trans = NULL;
    g_signal_emit_by_name(s->webrtcbin, "add-transceiver",
        GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, video_caps, &video_trans);
    if (video_trans) {
        g_object_set(video_trans, "codec-preferences", video_pref_caps, NULL);
        gst_object_unref(video_trans);
    }
    gst_caps_unref(video_caps);
    gst_caps_unref(video_pref_caps);

    if (s->enable_data_channel) {
        GObject *data_channel = NULL;
        g_signal_emit_by_name(s->webrtcbin, "create-data-channel", "datachannel", NULL, &data_channel);
        if (data_channel) g_object_unref(data_channel);
    }

    g_signal_connect(s->webrtcbin, "pad-added",                    G_CALLBACK(on_pad_added),                   s);
    g_signal_connect(s->webrtcbin, "on-ice-candidate",             G_CALLBACK(on_ice_candidate),               s);
    g_signal_connect(s->webrtcbin, "notify::connection-state",     G_CALLBACK(on_connection_state_change),     s);
    g_signal_connect(s->webrtcbin, "notify::ice-connection-state", G_CALLBACK(on_ice_connection_state_change), s);
    g_signal_connect(s->webrtcbin, "notify::ice-gathering-state",  G_CALLBACK(on_ice_gathering_state_change),  s);

    GstStateChangeReturn ret = gst_element_set_state(s->pipeline, GST_STATE_PLAYING);
    LOG("[%" G_GINT64_FORMAT "] pipeline set to PLAYING: %s", s->id,
        ret == GST_STATE_CHANGE_SUCCESS    ? "SUCCESS"    :
        ret == GST_STATE_CHANGE_ASYNC      ? "ASYNC"      :
        ret == GST_STATE_CHANGE_NO_PREROLL ? "NO_PREROLL" : "FAILURE");

    LOG("[%" G_GINT64_FORMAT "] triggering create-offer", s->id);
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, s, NULL);
    g_signal_emit_by_name(s->webrtcbin, "create-offer", NULL, promise);

    return G_SOURCE_REMOVE;
}

// ─── Public API ──────────────────────────────────────────────────────────────

struct webrtc_session *webrtc_session_new(
    bool enable_data_channel,
    webrtc_session_offer_ready_cb on_offer_ready,
    webrtc_session_error_cb on_error,
    webrtc_session_new_sample_cb on_new_sample,
    webrtc_session_connection_failed_cb on_connection_failed,
    void *userdata
) {
    ensure_glib_running();

    struct webrtc_session *s = g_new0(struct webrtc_session, 1);
    if (!s) return NULL;
    s->id = s_next_id++;
    s->enable_data_channel = enable_data_channel ? TRUE : FALSE;
    s->offer_ready_cb = on_offer_ready;
    s->error_cb = on_error;
    s->new_sample_cb = on_new_sample;
    s->connection_failed_cb = on_connection_failed;
    s->userdata = userdata;

    LOG("[%" G_GINT64_FORMAT "] session created (enable_data_channel=%s) — setup queued",
        s->id, s->enable_data_channel ? "true" : "false");
    g_idle_add(do_session_setup, s);
    return s;
}

static void on_set_remote_description_done(GstPromise *promise, gpointer data) {
    (void) data;
    gst_promise_wait(promise);
    gst_promise_unref(promise);
}

int webrtc_session_set_answer(struct webrtc_session *s, const char *answer_sdp) {
    if (!s || !answer_sdp) return -1;
    if (!s->webrtcbin) {
        LOG("[%" G_GINT64_FORMAT "] set_answer: pipeline not ready", s->id);
        return -1;
    }

    // Match the FFI's defensive checks so we fail fast on rejected video.
    const char *video_mline = strstr(answer_sdp, "m=video ");
    if (!video_mline) {
        LOG("[%" G_GINT64_FORMAT "] set_answer: answer has no video m-line", s->id);
        return -1;
    }
    if (strncmp(video_mline, "m=video 0", 9) == 0) {
        LOG("[%" G_GINT64_FORMAT "] set_answer: answer rejected video m-line", s->id);
        return -1;
    }
    const char *vp8 = strstr(answer_sdp, "a=rtpmap:96 VP8/90000");
    const char *vp9 = strstr(answer_sdp, "a=rtpmap:98 VP9/90000");
    const char *h264 = strstr(answer_sdp, "H264/90000");
    const char *h265 = strstr(answer_sdp, "H265/90000");

    // Some Nest responses leave m=video active (port 9) but list only payload
    // 0 with no usable video rtpmap. Treat as video rejected.
    const char *video_line_end = strchr(video_mline, '\n');
    size_t video_line_len = video_line_end ? (size_t)(video_line_end - video_mline) : strlen(video_mline);
    gchar *video_line = g_strndup(video_mline, video_line_len);
    gboolean video_only_pt0 = (strstr(video_line, " UDP/TLS/RTP/SAVPF 0") != NULL);
    g_free(video_line);

    if (video_only_pt0 || (!vp8 && !vp9 && !h264 && !h265)) {
        LOG("[%" G_GINT64_FORMAT "] set_answer: answer has no usable video codec (pt0_only=%d)",
            s->id, video_only_pt0 ? 1 : 0);
        return -1;
    }

    GstSDPMessage *sdp_msg = NULL;
    if (gst_sdp_message_new_from_text(answer_sdp, &sdp_msg) != GST_SDP_OK) {
        LOG("[%" G_GINT64_FORMAT "] set_answer: failed to parse answer SDP", s->id);
        return -1;
    }
    GstWebRTCSessionDescription *answer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp_msg);

    GstPromise *promise = gst_promise_new_with_change_func(on_set_remote_description_done, s, NULL);
    g_signal_emit_by_name(s->webrtcbin, "set-remote-description", answer, promise);
    gst_webrtc_session_description_free(answer);
    LOG("[%" G_GINT64_FORMAT "] set_answer: remote description submitted", s->id);
    return 0;
}

// Tear down on the GLib thread to keep all GStreamer ops in one place.
struct destroy_ctx { struct webrtc_session *s; };

static gboolean do_session_destroy(gpointer data) {
    struct destroy_ctx *ctx = data;
    struct webrtc_session *s = ctx->s;
    LOG("[%" G_GINT64_FORMAT "] destroy", s->id);

    if (s->pli_timer_source != 0) {
        g_source_remove(s->pli_timer_source);
        s->pli_timer_source = 0;
    }
    if (s->ice_timeout_source != 0) {
        g_source_remove(s->ice_timeout_source);
        s->ice_timeout_source = 0;
    }
    if (s->pipeline) {
        gst_element_set_state(s->pipeline, GST_STATE_NULL);
        gst_object_unref(s->pipeline);
    }
    g_free(s);
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

void webrtc_session_destroy(struct webrtc_session *s) {
    if (!s) return;
    struct destroy_ctx *ctx = g_new0(struct destroy_ctx, 1);
    ctx->s = s;
    g_idle_add(do_session_destroy, ctx);
}
