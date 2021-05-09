/* soup-message-io-http2.c
 *
 * Copyright 2021 Igalia S.L.
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "libsoup-http2"

#include <glib.h>
#include <glib/gi18n-lib.h>

#include "soup-client-message-io-http2.h"

#include "soup-body-input-stream.h"
#include "soup-message-metrics-private.h"
#include "soup-message-private.h"
#include "soup-message-io-source.h"
#include "soup-message-queue-item.h"
#include "content-sniffer/soup-content-sniffer-stream.h"
#include "soup-client-input-stream.h"
#include "soup-logger-private.h"
#include "soup-uri-utils-private.h"

#include "content-decoder/soup-content-decoder.h"
#include "soup-memory-input-stream.h"

#include <nghttp2/nghttp2.h>

#define FRAME_HEADER_SIZE 9

typedef enum {
        STATE_NONE,
        STATE_WRITE_HEADERS,
        STATE_WRITE_DATA,
        STATE_WRITE_DONE,
        STATE_READ_HEADERS,
        STATE_READ_DATA,
        STATE_READ_DATA_SNIFFED,
        STATE_READ_DONE,
        STATE_ERROR,
} SoupHTTP2IOState;

typedef struct {
        SoupClientMessageIO iface;

        GIOStream *stream;
        GInputStream *istream;
        GOutputStream *ostream;

        GMainContext *async_context;

        GPtrArray *messages;
        GHashTable *message_errors;

        nghttp2_session *session;

        // Owned by nghttp2
        guint8 *write_buffer;
        gssize write_buffer_size;
        gssize written_bytes;

        gboolean is_shutdown;
} SoupMessageIOHTTP2;

typedef struct {
        SoupMessageQueueItem *item;
        SoupMessage *msg;
        SoupMessageMetrics *metrics;
        GCancellable *cancellable;
        GInputStream *decoded_data_istream;
        GInputStream *memory_data_istream;

        // Request body logger
        SoupLogger *logger;

        // Both data sources
        GCancellable *data_source_cancellable;

        // Pollable data sources
        GSource *data_source_poll;

        // Non-pollable data sources
        GByteArray *data_source_buffer;
        GError *data_source_error;
        gboolean data_source_eof;

        GSource *io_source;
        SoupMessageIOHTTP2 *io; // Unowned
        SoupMessageIOCompletionFn completion_cb;
        gpointer completion_data;
        SoupHTTP2IOState state;
        gboolean paused;
        guint32 stream_id;
} SoupHTTP2MessageData;

static void soup_message_io_http2_finished (SoupClientMessageIO *, SoupMessage *);
static gboolean io_read_or_write (SoupMessageIOHTTP2 *, gboolean, GCancellable *, GError **);

#if 0
static SoupHTTP2MessageData *
get_message_by_stream_id (SoupMessageIOHTTP2 *io, guint32 stream_id)
{
        const guint len = io->messages->len;

        for (uint i = 0; i < len; ++i) {
                SoupHTTP2MessageData *data = io->messages->pdata[i];
                if (data->stream_id == stream_id)
                        return data;
        }

        if (stream_id != 0)
                g_warning ("Recieved frame for unknown stream id %u!", stream_id);
        return NULL;
}
#endif

static const char *
frame_type_to_string (nghttp2_frame_type type)
{
        switch (type) {
        case NGHTTP2_DATA:
                return "DATA";
        case NGHTTP2_HEADERS:
                return "HEADERS";
        case NGHTTP2_PRIORITY:
                return "PRIORITY";
        case NGHTTP2_RST_STREAM:
                return "RST_STREAM";
        case NGHTTP2_SETTINGS:
                return "SETTINGS";
        case NGHTTP2_PUSH_PROMISE:
                return "PUSH_PROMISE";
        case NGHTTP2_PING:
                return "PING";
        case NGHTTP2_GOAWAY:
                return "GOAWAY";
        case NGHTTP2_WINDOW_UPDATE:
                return "WINDOW_UPDATE";
        case NGHTTP2_CONTINUATION:
                return "CONTINUATION";
        case NGHTTP2_ALTSVC:
                return "ALTSVC";
        case NGHTTP2_ORIGIN:
                return "ORIGIN";
        default:
                g_warn_if_reached ();
                return "UNKNOWN";
        }
}

static void
h2_debug (SoupMessageIOHTTP2 *io, SoupHTTP2MessageData *data, const char *format, ...)
{
        va_list args;
        char *message;
        guint32 stream_id = 0;

	va_start (args, format);
	message = g_strdup_vprintf (format, args);
	va_end (args);

        if (data)
                stream_id = data->stream_id;

        g_assert (io);
        g_log (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "[C%p-S%u] %s", io->stream, stream_id, message);

        g_free (message);
}

static SoupMessageIOHTTP2 *
get_io_data (SoupMessage *msg)
{
        return (SoupMessageIOHTTP2 *)soup_message_get_io_data (msg);
}

static int
get_data_io_priority (SoupHTTP2MessageData *data)
{
	if (!data->item->task)
		return G_PRIORITY_DEFAULT;

	return g_task_get_priority (data->item->task);
}

static void
set_error_for_data (SoupMessageIOHTTP2 *io, SoupHTTP2MessageData *data, GError *error)
{
        g_debug ("set_error_for_data: %s", error->message);
        g_hash_table_replace (io->message_errors, data, error);
}

static GError *
get_error_for_data (SoupMessageIOHTTP2 *io, SoupHTTP2MessageData *data)
{
        GError *error = NULL;

        g_hash_table_steal_extended (io->message_errors, data, NULL, (gpointer*)&error);

        return error;
}

/* HTTP2 read callbacks */

static int
on_header_callback (nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);

        if (!data)
                return 0;

        SoupMessage *msg = data->msg;
        if (name[0] == ':') {
                if (strcmp ((char *)name, ":status") == 0) {
                        guint status_code = (guint)g_ascii_strtoull ((char *)value, NULL, 10);
                        soup_message_set_status (msg, status_code, NULL);
                        return 0;
                }
                g_debug ("%s = %s", name, value);
                return 0;
        }

        // FIXME: Encoding
        char *name_utf8 = g_utf8_make_valid ((const char *)name, namelen);
        char *value_utf8 = g_utf8_make_valid ((const char *)value, valuelen);
        soup_message_headers_append (soup_message_get_response_headers (data->msg),
                                     name_utf8, value_utf8);
        g_free (name_utf8);
        g_free (value_utf8);
        return 0;
}

static GError *
memory_stream_want_read_callback (SoupMemoryInputStream *stream, GCancellable *cancellable, gboolean blocking, gpointer user_data)
{
        SoupHTTP2MessageData *data = (SoupHTTP2MessageData*)user_data;
        GError *error = NULL;

        g_debug ("memory_stream_want_read_callback write=%d read=%d", nghttp2_session_want_write (data->io->session), nghttp2_session_want_read (data->io->session));

        io_read_or_write (data->io, blocking, cancellable, &error);

        return error;
}

static int
on_begin_frame_callback (nghttp2_session *session, const nghttp2_frame_hd *hd, void *user_data)
{
        // SoupMessageIOHTTP2 *io = user_data;
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, hd->stream_id);

        h2_debug (user_data, data, "[RECV] [%s] Beginning", frame_type_to_string (hd->type));

        if (!data)
                return 0;

        switch (hd->type) {
        case NGHTTP2_HEADERS:
                if (data->state < STATE_READ_HEADERS)
                        data->state = STATE_READ_HEADERS;
                break;
        case NGHTTP2_DATA: {
                // We may have sniffed a previous DATA frame
                if (data->state < STATE_READ_DATA)
                        data->state = STATE_READ_DATA;
                if (!data->memory_data_istream) {
                        data->memory_data_istream = soup_memory_input_stream_new (G_POLLABLE_INPUT_STREAM (data->io->istream));
                        g_signal_connect (data->memory_data_istream, "want-read",
                                          G_CALLBACK (memory_stream_want_read_callback), data);
                }
                if (!data->decoded_data_istream)
                        data->decoded_data_istream = soup_message_setup_body_istream (data->memory_data_istream, data->msg,
                                                                                      data->item->session, SOUP_STAGE_MESSAGE_BODY);
                break;
        }
        }

        return 0;
}

static void
handle_goaway (SoupMessageIOHTTP2 *io, guint32 error_code, guint32 last_stream_id)
{
        const guint len = io->messages->len;

        for (uint i = 0; i < len; ++i) {
                SoupHTTP2MessageData *data = io->messages->pdata[i];
                /* If there is no error it is a graceful shutdown and
                 * existing messages can be handled otherwise it is a fatal error */
                if ((error_code == 0 && data->stream_id > last_stream_id) ||
                     data->state < STATE_READ_DONE) {
                        h2_debug (io, data, "[GOAWAY] Error: %s", nghttp2_http2_strerror (error_code));
                        data->state = STATE_ERROR;
                        // TODO: We can restart unfinished messages
                        set_error_for_data (io, data, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                            "HTTP/2 Error: %s", nghttp2_http2_strerror (error_code)));
                }
        }
}

static int
on_frame_recv_callback (nghttp2_session *session, const nghttp2_frame *frame, gpointer user_data)
{
        SoupMessageIOHTTP2 *io = user_data;
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);

        h2_debug (io, data, "[RECV] [%s] Recieved (%u)", frame_type_to_string (frame->hd.type), frame->hd.flags);

        if (frame->hd.type == NGHTTP2_GOAWAY) {
                handle_goaway (io, frame->goaway.error_code, frame->goaway.last_stream_id);
                io->is_shutdown = TRUE;
                return 0;
        }

        if (!data) {
                if (frame->hd.stream_id != 0 && !(frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
                        g_warn_if_reached ();
                return 0;
        }

        switch (frame->hd.type) {
        case NGHTTP2_HEADERS:
                if (data->metrics)
                        data->metrics->response_header_bytes_received += frame->hd.length + FRAME_HEADER_SIZE;

                if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE && frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) {
                        h2_debug (io, data, "[HEADERS] status %u", soup_message_get_status (data->msg));
                        if (SOUP_STATUS_IS_INFORMATIONAL (soup_message_get_status (data->msg))) {
                                soup_message_got_informational (data->msg);
                                soup_message_cleanup_response (data->msg);
                                soup_memory_input_stream_complete (SOUP_MEMORY_INPUT_STREAM (data->memory_data_istream));
                                data->state = STATE_READ_DONE;
                                break;
                        } else if (soup_message_get_status (data->msg) == SOUP_STATUS_NO_CONTENT) {
                                data->state = STATE_READ_DONE;
                        }
                        soup_message_got_headers (data->msg);
                }
                break;
        case NGHTTP2_DATA:
                if (data->metrics)
                        data->metrics->response_body_bytes_received += frame->data.hd.length + FRAME_HEADER_SIZE;
                break;
        case NGHTTP2_RST_STREAM:
                h2_debug (io, data, "[RST_STREAM] %s", nghttp2_http2_strerror (frame->rst_stream.error_code));
                if (frame->rst_stream.error_code != NGHTTP2_NO_ERROR) {
                        set_error_for_data (io, data, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                                           nghttp2_http2_strerror (frame->rst_stream.error_code)));
                        data->state = STATE_ERROR;
                }
                break;
        };

        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                h2_debug (io, data, "Stream done");
                data->state = STATE_READ_DONE;
                if (frame->hd.type == NGHTTP2_DATA) {
                        soup_memory_input_stream_complete (SOUP_MEMORY_INPUT_STREAM (data->memory_data_istream));
                        soup_message_got_body (data->msg);
                }

                // soup_message_io_http2_finished (data->msg);
                // nghttp2_submit_rst_stream (session, NGHTTP2_FLAG_NONE, frame->hd.stream_id, NGHTTP2_STREAM_CLOSED);
        }

        return 0;
}

static int
on_data_chunk_recv_callback (nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
        SoupMessageIOHTTP2 *io = user_data;
        SoupHTTP2MessageData *msgdata = nghttp2_session_get_stream_user_data (session, stream_id);

        if (!msgdata)
                return NGHTTP2_ERR_CALLBACK_FAILURE;

        h2_debug (io, msgdata, "[DATA] Recieved chunk, len=%zu, flags=%u, paused=%d", len, flags, msgdata->paused);

        if (msgdata->paused)
                return NGHTTP2_ERR_PAUSE;

        SoupMessage *msg = msgdata->msg;
        GBytes *bytes = g_bytes_new (data, len);
        g_assert (msgdata->memory_data_istream != NULL);
        soup_memory_input_stream_add_bytes (SOUP_MEMORY_INPUT_STREAM (msgdata->memory_data_istream), bytes);
        g_bytes_unref (bytes);

        if (msgdata->state < STATE_READ_DATA_SNIFFED) {
                if (soup_message_get_content_sniffer (msg)) {
                        SoupContentSnifferStream *sniffer_stream = SOUP_CONTENT_SNIFFER_STREAM (msgdata->decoded_data_istream);
                        GError *error = NULL;
                        if (soup_content_sniffer_stream_is_ready (sniffer_stream, FALSE, NULL, &error)) {
                                GHashTable *params;
                                const char *content_type = soup_content_sniffer_stream_sniff (sniffer_stream, &params);

                                msgdata->state = STATE_READ_DATA_SNIFFED;
                                soup_message_content_sniffed (msg, content_type, params);
                                h2_debug (io, msgdata, "[DATA] Sniffed %s", content_type);
                        } else {
                                h2_debug (io, msgdata, "[DATA] Sniffer stream was not ready %s", error->message);
                                g_clear_error (&error);
                        }
                }
                else
                        msgdata->state = STATE_READ_DATA_SNIFFED;
        }

        return 0;
}

/* HTTP2 write callbacks */

static int
on_before_frame_send_callback (nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);

        // h2_debug (user_data, data, "[SEND] [%s] Before", frame_type_to_string (frame->hd.type));

        if (!data)
                return 0;

        switch (frame->hd.type) {
        case NGHTTP2_HEADERS:
                data->state = STATE_WRITE_HEADERS;
                break;
        case NGHTTP2_DATA:
                data->state = STATE_WRITE_DATA;
                break;
        }

        return 0;
}

static int
on_frame_send_callback (nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);

        if (!data) {
                h2_debug (user_data, NULL, "[SEND] [%s]", frame_type_to_string (frame->hd.type));
                return 0;
        }

        switch (frame->hd.type) {
        case NGHTTP2_HEADERS:
                h2_debug (user_data, data, "[SEND] [HEADERS] finished=%d",
                          (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) ? 1 : 0);
                if (data->metrics)
                        data->metrics->request_header_bytes_sent += frame->hd.length + FRAME_HEADER_SIZE;

                if (frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) {
                        soup_message_wrote_headers (data->msg);
                        if (soup_message_get_request_body_stream (data->msg) == NULL) {
                                data->state = STATE_WRITE_DONE;
                                soup_message_wrote_body (data->msg);
                        }
                }
                break;
        case NGHTTP2_DATA:
                h2_debug (user_data, data, "[SEND] [DATA] bytes=%zu, finished=%d",
                          frame->data.hd.length, frame->hd.flags & NGHTTP2_FLAG_END_STREAM);
                if (data->metrics) {
                        data->metrics->request_body_bytes_sent += frame->hd.length + FRAME_HEADER_SIZE;
                        data->metrics->request_body_size += frame->data.hd.length;
                }
                if (frame->data.hd.length)
                        soup_message_wrote_body_data (data->msg, frame->data.hd.length);
                if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                        data->state = STATE_WRITE_DONE;
                        soup_message_wrote_body (data->msg);
                }
                break;
        default:
                h2_debug (user_data, NULL, "[SEND] [%s]", frame_type_to_string (frame->hd.type));
                break;
        }

        return 0;
}

static int
on_frame_not_send_callback (nghttp2_session *session, const nghttp2_frame *frame, int lib_error_code, void *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);

        h2_debug (user_data, data, "[SEND] [%s] Failed", frame_type_to_string (frame->hd.type));

        if (!data)
                return 0;

        data->state = STATE_ERROR;

        return 0;
}

static int
on_stream_close_callback (nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
        g_debug ("on_stream_close %d", stream_id);
        return 0;
}

static gboolean
on_data_readable (GInputStream *stream, gpointer user_data)
{
        SoupHTTP2MessageData *data = (SoupHTTP2MessageData*)user_data;

        nghttp2_session_resume_data (data->io->session, data->stream_id);

        g_clear_pointer (&data->data_source_poll, g_source_unref);
        return G_SOURCE_REMOVE;
}

static void
on_data_read (GInputStream *source, GAsyncResult *res, gpointer user_data)
{
        SoupHTTP2MessageData *data = user_data;
        GError *error = NULL;
        gssize read = g_input_stream_read_finish (source, res, &error);

        g_debug ("[SEND_BODY] Read %zu", read);

        // This operation may have outlived the message data in which
        // case this will have been cancelled.
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                g_error_free (error);
                return;
        }

        if (read < 0) {
                g_byte_array_set_size (data->data_source_buffer, 0);
                data->data_source_error = g_steal_pointer (&error);
        } else if (read == 0)
                data->data_source_eof = TRUE;
        else
                g_byte_array_set_size (data->data_source_buffer, read);

        g_debug ("[SEND_BODY] Resuming send");
        nghttp2_session_resume_data (data->io->session, data->stream_id);
}

static void
log_request_data (SoupHTTP2MessageData *data, const guint8 *buffer, gsize len)
{
        if (!data->logger)
                return;

        // NOTE: This doesn't exactly log data as it hits the network but
        // rather as soon as we read it from our source which is as good
        // as we can do since nghttp handles the actual io.
        soup_logger_log_request_data (data->logger, data->msg, (const char *)buffer, len);
}

static ssize_t
on_data_source_read_callback (nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, stream_id);
        SoupMessageIOHTTP2 *io = get_io_data (data->msg);

        if (data->paused) {
                g_debug ("[SEND_BODY] Paused");
                return NGHTTP2_ERR_PAUSE;
        }

        /* This cancellable is only used for async data source operations,
         * only exists while reading is happening, and will be cancelled
         * at any point if the data is freed.
         */
        if (!data->data_source_cancellable)
                data->data_source_cancellable = g_cancellable_new ();

        /* We support pollable streams in the best case because they
         * should perform better with one fewer copy of each buffer and no threading. */
        if (G_IS_POLLABLE_INPUT_STREAM (source->ptr) && g_pollable_input_stream_can_poll (G_POLLABLE_INPUT_STREAM (source->ptr))) {
                GPollableInputStream *in_stream = G_POLLABLE_INPUT_STREAM (source->ptr);
                GError *error = NULL;

                gssize read = g_pollable_input_stream_read_nonblocking  (in_stream, buf, length, data->cancellable, &error);

                if (read) {
                        g_debug ("[SEND_BODY] Read %zu", read);
                        log_request_data (data, buf, read);
                }

                if (read < 0) {
                        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                                g_assert (data->data_source_poll == NULL);

                                g_debug ("[SEND_BODY] Polling");
                                data->data_source_poll = g_pollable_input_stream_create_source (in_stream, data->data_source_cancellable);
                                g_source_set_callback (data->data_source_poll, (GSourceFunc)on_data_readable, data, NULL);
                                g_source_set_priority (data->data_source_poll, get_data_io_priority (data));
                                g_source_attach (data->data_source_poll, g_main_context_get_thread_default ());

                                g_error_free (error);
                                return NGHTTP2_ERR_DEFERRED;
                        }

                        g_debug ("[SEND_BODY] Error %s", error->message);
                        set_error_for_data (io, data, g_steal_pointer (&error));
                        data->state = STATE_ERROR;
                        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
                }
                else if (read == 0) {
                        g_debug ("[SEND_BODY] EOF");
                        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
                }

                return read;
        } else {
                GInputStream *in_stream = G_INPUT_STREAM (source->ptr);

                /* To support non-pollable input streams we always deffer reads
                * and read async into a local buffer. The next time around we will
                * send that buffer or error.
                */
                if (!data->data_source_buffer)
                        data->data_source_buffer = g_byte_array_new ();

                gsize buffer_len = data->data_source_buffer->len;
                if (buffer_len) {
                        g_debug ("[SEND_BODY] Sending %zu", buffer_len);
                        g_assert (buffer_len <= length); // QUESTION: Maybe not reliable
                        memcpy (buf, data->data_source_buffer->data, buffer_len);
                        log_request_data (data, buf, buffer_len);
                        g_byte_array_set_size (data->data_source_buffer, 0);
                        return buffer_len;
                } else if (data->data_source_eof) {
                        g_debug ("[SEND_BODY] EOF");
                        g_clear_object (&data->data_source_cancellable);
                        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
                        return 0;
                } else if (data->data_source_error) {
                        g_debug ("[SEND_BODY] Error %s", data->data_source_error->message);
                        g_clear_object (&data->data_source_cancellable);
                        set_error_for_data (io, data, g_steal_pointer (&data->data_source_error));
                        data->state = STATE_ERROR;
                        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
                } else {
                        g_debug ("[SEND_BODY] Reading async");
                        g_byte_array_set_size (data->data_source_buffer, length);
                        g_input_stream_read_async (in_stream, data->data_source_buffer->data, length,
                                                   get_data_io_priority (data),
                                                   data->data_source_cancellable,
                                                   (GAsyncReadyCallback)on_data_read, data);
                        return NGHTTP2_ERR_DEFERRED;
                }
        }
}

/* HTTP2 IO functions */

static gboolean
data_compare (gconstpointer a, gconstpointer b)
{
        SoupHTTP2MessageData *data1 = (SoupHTTP2MessageData *)a, *data2 = (SoupHTTP2MessageData *)b;

        return data1->msg == data2->msg;
}

static SoupHTTP2MessageData *
add_message_to_io_data (SoupMessageIOHTTP2 *io,
                        SoupMessageQueueItem *item,
                        SoupMessageIOCompletionFn completion_cb,
                        gpointer completion_data)
{
        SoupHTTP2MessageData *data = g_new0 (SoupHTTP2MessageData, 1);

        data->item = soup_message_queue_item_ref (item);
        data->msg = item->msg;
        data->metrics = soup_message_get_metrics (data->msg);
        data->cancellable = item->cancellable;
        data->completion_cb = completion_cb;
        data->completion_data = completion_data;
        data->stream_id = 0; // Will be overwritten
        data->io = io;

        if (g_ptr_array_find_with_equal_func (io->messages, data, data_compare, NULL))
                g_warn_if_reached ();
        g_ptr_array_add (io->messages, data);

        return data;
}

static void
soup_http2_message_data_free (SoupHTTP2MessageData *data)
{
        g_clear_pointer (&data->item, soup_message_queue_item_unref);
        g_clear_object (&data->memory_data_istream);
        g_clear_object (&data->decoded_data_istream);

        if (data->io_source) {
                g_source_destroy (data->io_source);
                g_clear_pointer (&data->io_source, g_source_unref);
        }

        if (data->data_source_poll)
                g_source_destroy (data->data_source_poll);
        g_clear_pointer (&data->data_source_poll, g_source_unref);

        g_clear_error (&data->data_source_error);
        g_clear_pointer (&data->data_source_buffer, g_byte_array_unref);

        if (data->data_source_cancellable) {
                g_cancellable_cancel (data->data_source_cancellable);
                g_clear_object (&data->data_source_cancellable);
        }

        g_free (data);
}

#define MAKE_NV(NAME, VALUE, VALUELEN)                                      \
        {                                                                   \
                (uint8_t *)NAME, (uint8_t *)VALUE, strlen (NAME), VALUELEN, \
                    NGHTTP2_NV_FLAG_NONE                                    \
        }

#define MAKE_NV2(NAME, VALUE)                                                     \
        {                                                                         \
                (uint8_t *)NAME, (uint8_t *)VALUE, strlen (NAME), strlen (VALUE), \
                    NGHTTP2_NV_FLAG_NONE                                          \
        }

#define MAKE_NV3(NAME, VALUE, FLAGS)                                              \
        {                                                                         \
                (uint8_t *)NAME, (uint8_t *)VALUE, strlen (NAME), strlen (VALUE), \
                    FLAGS                                                         \
        }

static void
send_message_request (SoupMessage *msg, SoupMessageIOHTTP2 *io, SoupHTTP2MessageData *data)
{
        GArray *headers = g_array_new (FALSE, FALSE, sizeof (nghttp2_nv));

        GUri *uri = soup_message_get_uri (msg);
        char *host = soup_uri_get_host_for_headers (uri);
        char *authority = g_strdup_printf ("%s:%u", host, g_uri_get_port (uri));

        char *path_and_query;
        if (soup_message_get_is_options_ping (msg))
                path_and_query = g_strdup ("*");
        else
                path_and_query = g_strdup_printf ("%s%c%s", g_uri_get_path (uri), g_uri_get_query (uri) ? '?' : '\0', g_uri_get_query (uri));

        const nghttp2_nv pseudo_headers[] = {
                MAKE_NV3 (":method", soup_message_get_method (msg), NGHTTP2_NV_FLAG_NO_COPY_VALUE),
                MAKE_NV2 (":scheme", g_uri_get_scheme (uri)),
                MAKE_NV2 (":authority", authority),
                MAKE_NV2 (":path", path_and_query),
        };

        for (guint i = 0; i < G_N_ELEMENTS (pseudo_headers); ++i) {
                g_array_append_val (headers, pseudo_headers[i]);
        }

        SoupMessageHeadersIter iter;
        const char *name, *value;
        soup_message_headers_iter_init (&iter, soup_message_get_request_headers (msg));
        while (soup_message_headers_iter_next (&iter, &name, &value)) {
                /* Forbidden headers. TODO: Avoid setting this elsewhere? */
                if (g_ascii_strcasecmp (name, "Transfer-Encoding") == 0)
                        continue;
                const nghttp2_nv nv = MAKE_NV2 (name, value);
                g_array_append_val (headers, nv);
        }

        GInputStream *body_stream = soup_message_get_request_body_stream (msg);
        SoupSessionFeature *logger = soup_session_get_feature_for_message (data->item->session, SOUP_TYPE_LOGGER, data->msg);
        if (logger && body_stream)
                data->logger = SOUP_LOGGER (logger);

        nghttp2_data_provider *data_provider = NULL;
        if (body_stream) {
                data_provider = g_new (nghttp2_data_provider, 1);
                data_provider->source.ptr = body_stream;
                data_provider->read_callback = on_data_source_read_callback;
        }

        data->stream_id = nghttp2_submit_request (io->session, NULL, (const nghttp2_nv *)headers->data, headers->len, data_provider, data);

        h2_debug (io, data, "[SESSION] Request made for %s%s", authority, path_and_query);

        g_array_free (headers, TRUE);
        g_free (authority);
        g_free (host);
        g_free (path_and_query);
        g_free (data_provider);
}



static void
soup_message_io_http2_send_item (SoupClientMessageIO *iface,
                                 SoupMessageQueueItem *item,
                                 SoupMessageIOCompletionFn completion_cb,
                                 gpointer user_data)
{
        SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = add_message_to_io_data (io, item, completion_cb, user_data);

        send_message_request (item->msg, io, data);
}

static SoupHTTP2MessageData *
get_data_for_message (SoupMessageIOHTTP2 *io, SoupMessage *msg)
{
        const guint len = io->messages->len;

        for (uint i = 0; i < len; ++i) {
                SoupHTTP2MessageData *data = io->messages->pdata[i];
                if (data->msg == msg)
                        return data;
        }

        g_warn_if_reached ();
        return NULL;
}

static void
soup_message_io_http2_finished (SoupClientMessageIO *iface,
                                SoupMessage         *msg)
{
        SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data;
	SoupMessageIOCompletionFn completion_cb;
	gpointer completion_data;
	SoupMessageIOCompletion completion;

        data = get_data_for_message (io, msg);

        h2_debug (io, data, "Finished");

        // int ret;
        // ret = nghttp2_submit_rst_stream (io->session, NGHTTP2_FLAG_NONE, data->stream_id, NGHTTP2_STREAM_CLOSED);
        // g_assert (ret == 0);
        // ret = nghttp2_session_terminate_session (io->session, NGHTTP2_NO_ERROR);
        // g_assert (ret == 0);

	completion_cb = data->completion_cb;
	completion_data = data->completion_data;

	// TODO
	completion = SOUP_MESSAGE_IO_COMPLETE;

	g_object_ref (msg);

        nghttp2_session_set_stream_user_data (io->session, data->stream_id, NULL);
        if (!g_ptr_array_remove_fast (io->messages, data))
                g_warn_if_reached ();

        soup_connection_message_io_finished (soup_message_get_connection (msg), msg);

	if (completion_cb)
		completion_cb (G_OBJECT (msg), completion, completion_data);

	g_object_unref (msg);
}

static void
soup_message_io_http2_pause (SoupClientMessageIO *iface,
                             SoupMessage         *msg)
{
        SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        g_debug ("soup_message_io_http2_pause");

        if (data->paused)
                g_warn_if_reached ();

        data->paused = TRUE;
}

static void
soup_message_io_http2_unpause (SoupClientMessageIO *iface,
                               SoupMessage         *msg)
{
        SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        g_debug ("soup_message_io_http2_unpause");

        if (!data->paused)
                g_warn_if_reached ();

        data->paused = FALSE;
}

static void
soup_message_io_http2_stolen (SoupClientMessageIO *iface)
{
        g_assert_not_reached ();
}

static gboolean
soup_message_io_http2_in_progress (SoupClientMessageIO *iface,
                                   SoupMessage         *msg)
{
        SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        return data->state < STATE_WRITE_DONE;
}

static gboolean
soup_message_io_http2_is_paused (SoupClientMessageIO *iface,
                                 SoupMessage         *msg)
{
        SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        return data->paused;
}

static gboolean
soup_message_io_http2_is_reusable (SoupClientMessageIO *iface)
{
        SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;

        // TODO: This logic is probably incomplete
        return !io->is_shutdown;
}

static gboolean
message_source_check (GSource *source)
{
	SoupMessageIOSource *message_source = (SoupMessageIOSource *)source;
        SoupMessage *msg = SOUP_MESSAGE (message_source->msg);
        SoupMessageIOHTTP2 *io = get_io_data (msg);
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        //QUESTION: What is the point of message_source->paused

        return !data->paused;
}

static GSource *
soup_message_io_http2_get_source (SoupMessage *msg,
                                  GCancellable *cancellable,
                                  SoupMessageIOSourceFunc callback,
                                  gpointer user_data)
{
        SoupMessageIOHTTP2 *io = get_io_data (msg);
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);
        //GPollableInputStream *istream;

        //g_debug ("soup_message_io_http2_get_source state=%u, paused=%d", data->state, data->paused);

        //g_debug ("Queue %lu", nghttp2_session_get_outbound_queue_size (io->session));

        GSource *base_source;
        // TODO: Handle mixing writes in
        if (data->paused)
                base_source = cancellable ? g_cancellable_source_new (cancellable) : NULL;
        else if (data->state < STATE_WRITE_DONE)
                base_source = g_pollable_output_stream_create_source (G_POLLABLE_OUTPUT_STREAM (io->ostream), cancellable);
        else if (data->state < STATE_READ_DONE)
                base_source = g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM (io->istream), cancellable);
        else
                g_assert_not_reached ();

        GSource *source = soup_message_io_source_new (base_source, G_OBJECT (msg), data->paused, message_source_check);
        g_source_set_callback (source, (GSourceFunc)callback, user_data, NULL);
        return source;
}

static void
soup_message_io_http2_skip_body (SoupClientMessageIO *iface,
                                 SoupMessage         *msg)
{
        SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        g_debug ("soup_message_io_http2_skip_body");

        g_assert (data->memory_data_istream);

        soup_memory_input_stream_complete (SOUP_MEMORY_INPUT_STREAM (data->memory_data_istream));
        data->state = STATE_READ_DONE;
        soup_message_got_body (data->msg);
}

#if 0
static int
idle_finish (gpointer user_data)
{
        SoupMessage *msg = user_data;
        soup_message_io_http2_finished (msg); // TODO: Smarter
        return G_SOURCE_REMOVE;
}
#endif

static void
client_stream_eof (SoupClientInputStream *stream, gpointer user_data)
{
	SoupMessage *msg = user_data;
	SoupMessageIOHTTP2 *io = get_io_data (msg);

        if (!io) {
                g_warn_if_reached (); // QUESTION: Probably fine
                return;
        }

        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        g_debug ("client_stream_eof %d", SOUP_STATUS_IS_REDIRECTION (soup_message_get_status (data->msg)));

        data->state = STATE_READ_DONE;
        // data->item->state = SOUP_MESSAGE_FINISHED;
        // soup_message_io_http2_finished (msg);
        // g_idle_add (idle_finish, msg); // TODO
}

static GInputStream *
soup_message_io_http2_get_response_istream (SoupClientMessageIO *iface,
                                            SoupMessage         *msg,
                                            GError **error)
{
        SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);
        GInputStream *client_stream, *base_stream;

        g_debug ("soup_message_io_http2_get_response_istream paused=%d", data->paused);

        if (data->decoded_data_istream)
                base_stream = g_object_ref (data->decoded_data_istream);
        else /* For example with status_code == SOUP_STATUS_NO_CONTENT */
                base_stream = g_memory_input_stream_new ();

        client_stream = soup_client_input_stream_new (base_stream, msg);
        g_signal_connect (client_stream, "eof", G_CALLBACK (client_stream_eof), msg);

        g_object_unref (base_stream);

        return client_stream;
}

static gboolean
io_read (SoupMessageIOHTTP2 *io,
         gboolean blocking,
	 GCancellable *cancellable,
         GError **error)
{
        guint8 buffer[8192];
        gssize read;
        int ret;

        if ((read = g_pollable_stream_read (io->istream, buffer, sizeof (buffer),
                                            blocking, cancellable, error)) < 0)
            return FALSE;

        ret = nghttp2_session_mem_recv (io->session, buffer, read);
        return ret != 0;
}

static gboolean
io_write (SoupMessageIOHTTP2 *io,
         gboolean blocking,
	 GCancellable *cancellable,
         GError **error)
{
        /* We must write all of nghttp2's buffer before we ask for more */

        if (io->written_bytes == io->write_buffer_size)
                io->write_buffer = NULL;

        if (io->write_buffer == NULL) {
                io->written_bytes = 0;
                io->write_buffer_size = nghttp2_session_mem_send (io->session, (const guint8**)&io->write_buffer);
                if (io->write_buffer_size == 0) {
                        /* Done */
                        io->write_buffer = NULL;
                        return TRUE;
                }
        }

        gssize ret = g_pollable_stream_write (io->ostream,
                                              io->write_buffer + io->written_bytes,
                                              io->write_buffer_size - io->written_bytes,
                                              blocking, cancellable, error);
        if (ret < 0)
                return FALSE;

        io->written_bytes += ret;
        return TRUE;
}

static gboolean
io_read_or_write (SoupMessageIOHTTP2 *io,
                  gboolean blocking,
                  GCancellable *cancellable,
                  GError **error)
{
        // TODO: This can possibly more inteligent about what actually needs
        // writing so we can prioritize better.
        if (nghttp2_session_want_write (io->session))
                return io_write (io, blocking, cancellable, error);
        return io_read (io, blocking, cancellable, error);
}

static gboolean
io_run_until (SoupMessage *msg,
              gboolean blocking,
	      SoupHTTP2IOState state,
	      GCancellable *cancellable,
              GError **error)
{
	SoupMessageIOHTTP2 *io = get_io_data (msg);
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);
	gboolean progress = TRUE, done;
	GError *my_error = NULL;

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return FALSE;
	else if (!io) {
		g_set_error_literal (error, G_IO_ERROR,
				     G_IO_ERROR_CANCELLED,
				     _("Operation was cancelled"));
		return FALSE;
	}

	g_object_ref (msg);

	while (progress && get_io_data (msg) == io && !data->paused && data->state < state) {
                progress = io_read_or_write (io, blocking, cancellable, &my_error);
	}

	if (my_error || (my_error = get_error_for_data (io, data))) {
		g_propagate_error (error, my_error);
		g_object_unref (msg);
		return FALSE;
        } else if (get_io_data (msg) != io) {
		g_set_error_literal (error, G_IO_ERROR,
				     G_IO_ERROR_CANCELLED,
				     _("Operation was cancelled"));
		g_object_unref (msg);
		return FALSE;
	}

	done = data->state >= state;

	if (!blocking && !done) {
		g_set_error_literal (error, G_IO_ERROR,
				     G_IO_ERROR_WOULD_BLOCK,
				     _("Operation would block"));
		g_object_unref (msg);
		return FALSE;
	}

	g_object_unref (msg);
	return done;
}


static gboolean
soup_message_io_http2_run_until_read (SoupClientMessageIO *iface,
                                      SoupMessage         *msg,
                                      GCancellable *cancellable,
                                      GError **error)
{
        //SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;
        return io_run_until (msg, TRUE, STATE_READ_DATA, cancellable, error);
}

static gboolean
soup_message_io_http2_run_until_finish (SoupClientMessageIO *iface,
                                        SoupMessage         *msg,
                                        gboolean             blocking,
                                        GCancellable        *cancellable,
                                        GError             **error)
{

        g_debug ("soup_message_io_http2_run_until_finish");

        //QUESTION: Prematurely end the stream, we don't need more than what we are getting
        //nghttp2_submit_rst_stream (io->session, NGHTTP2_FLAG_NONE, data->stream_id, NGHTTP2_STREAM_CLOSED);

        return io_run_until (msg, blocking, STATE_READ_DONE, cancellable, error);
}

static void
soup_message_io_http2_run (SoupClientMessageIO *iface,
                           SoupMessage         *msg,
		           gboolean             blocking)
{
        g_assert_not_reached ();
}

static void io_run_until_read_async (SoupMessage *msg,
                                     GTask       *task);

static gboolean
io_run_until_read_ready (SoupMessage *msg,
                         gpointer     user_data)
{
        GTask *task = user_data;

        io_run_until_read_async (msg, task);

        return G_SOURCE_REMOVE;
}

static void
io_run_until_read_async (SoupMessage *msg,
                         GTask       *task)
{
        SoupMessageIOHTTP2 *io = get_io_data (msg);
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);
        GError *error = NULL;

        if (data->io_source) {
                g_source_destroy (data->io_source);
                g_clear_pointer (&data->io_source, g_source_unref);
        }

        if (io_run_until (msg, FALSE,
                          STATE_READ_DATA,
                          g_task_get_cancellable (task),
                          &error)) {
                g_task_return_boolean (task, TRUE);
                g_object_unref (task);
                return;
        }

        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                g_error_free (error);
                data->io_source = soup_message_io_http2_get_source (msg, g_task_get_cancellable (task),
                                                                  (SoupMessageIOSourceFunc)io_run_until_read_ready,
                                                                  task);
		g_source_set_priority (data->io_source, g_task_get_priority (task));
                g_source_attach (data->io_source, io->async_context);
                return;
        }

        if (get_io_data (msg) == io)
                soup_message_io_http2_finished ((SoupClientMessageIO *)io, msg);
        else
                g_warn_if_reached ();

        g_task_return_error (task, error);
        g_object_unref (task);
}

static void
soup_message_io_http2_run_until_read_async (SoupClientMessageIO *iface,
                                            SoupMessage         *msg,
                                            int                  io_priority,
                                            GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data)
{
        GTask *task;

        task = g_task_new (msg, cancellable, callback, user_data);
	g_task_set_priority (task, io_priority);
        io_run_until_read_async (msg, task);
}

static gboolean
soup_message_io_http2_is_open (SoupClientMessageIO *iface)
{
        SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;
        gboolean ret = TRUE;

        GError *error = NULL;
        if (!io_read (io, FALSE, NULL, &error)) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
                        ret = FALSE;

                g_clear_error (&error);
        }

        h2_debug (io, NULL, "[SESSION] Open=%d", ret);

        return ret;
}

static void
soup_message_io_http2_destroy (SoupClientMessageIO *iface)
{
        SoupMessageIOHTTP2 *io = (SoupMessageIOHTTP2 *)iface;

        g_debug ("soup_message_io_http2_destroy");

        g_clear_object (&io->stream);
        g_clear_pointer (&io->async_context, g_main_context_unref);
        g_clear_pointer (&io->session, nghttp2_session_del);
        g_clear_pointer (&io->messages, g_ptr_array_unref);
        g_clear_pointer (&io->message_errors, g_hash_table_unref);

        g_free (io);
}

static const SoupClientMessageIOFuncs io_funcs = {
        soup_message_io_http2_destroy,
        soup_message_io_http2_finished,
        soup_message_io_http2_stolen,
        soup_message_io_http2_send_item,
        soup_message_io_http2_get_response_istream,
        soup_message_io_http2_pause,
        soup_message_io_http2_unpause,
        soup_message_io_http2_is_paused,
        soup_message_io_http2_run,
        soup_message_io_http2_run_until_read,
        soup_message_io_http2_run_until_read_async,
        soup_message_io_http2_run_until_finish,
        soup_message_io_http2_in_progress,
        soup_message_io_http2_skip_body,
        soup_message_io_http2_is_open,
        soup_message_io_http2_is_reusable
        // soup_message_io_http2_get_source
};

static void
soup_client_message_io_http2_init (SoupMessageIOHTTP2 *io)
{
        // FIXME: Abort on out of memory errors
        nghttp2_session_callbacks *callbacks;
        nghttp2_session_callbacks_new (&callbacks);
        nghttp2_session_callbacks_set_on_header_callback (callbacks, on_header_callback);
        nghttp2_session_callbacks_set_on_frame_recv_callback (callbacks, on_frame_recv_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback (callbacks, on_data_chunk_recv_callback);
        nghttp2_session_callbacks_set_on_begin_frame_callback (callbacks, on_begin_frame_callback);
        nghttp2_session_callbacks_set_before_frame_send_callback (callbacks, on_before_frame_send_callback);
        nghttp2_session_callbacks_set_on_frame_not_send_callback (callbacks, on_frame_not_send_callback);
        nghttp2_session_callbacks_set_on_frame_send_callback (callbacks, on_frame_send_callback);
        nghttp2_session_callbacks_set_on_stream_close_callback (callbacks, on_stream_close_callback);

        nghttp2_session_client_new (&io->session, callbacks, io);
        nghttp2_session_callbacks_del (callbacks);

        io->messages = g_ptr_array_new_full (1, (GDestroyNotify)soup_http2_message_data_free);
        /* Errors are stored separate as they have a longer lifetime than MessageData */
        io->message_errors = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_error_free);

        io->iface.funcs = &io_funcs;
}

#define INITIAL_WINDOW_SIZE (32 * 1024 * 1024) // 32MB matches other implementations
#define MAX_HEADER_TABLE_SIZE 65536 // Match size used by Chromium/Firefox

SoupClientMessageIO *
soup_client_message_io_http2_new (GIOStream *stream)
{
        SoupMessageIOHTTP2 *io = g_new0 (SoupMessageIOHTTP2, 1);
        soup_client_message_io_http2_init (io);

        io->stream = g_object_ref (stream);
        io->istream = g_io_stream_get_input_stream (io->stream);
        io->ostream = g_io_stream_get_output_stream (io->stream);

        io->async_context = g_main_context_ref_thread_default ();

        nghttp2_session_set_local_window_size (io->session, NGHTTP2_FLAG_NONE, 0, INITIAL_WINDOW_SIZE);

        const nghttp2_settings_entry settings[] = {
                { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, INITIAL_WINDOW_SIZE },
                { NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, MAX_HEADER_TABLE_SIZE },
                { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 },
        };
        nghttp2_submit_settings (io->session, NGHTTP2_FLAG_NONE, settings, G_N_ELEMENTS (settings));

        return (SoupClientMessageIO *)io;
}