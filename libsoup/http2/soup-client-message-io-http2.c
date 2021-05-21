/* soup-message-io-http2.c
 *
 * Copyright 2021 Igalia S.L.
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
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
 * SPDX-License-Identifier: LGPL-2.0-or-later
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
#include "soup-body-input-stream-http2.h"

#include <nghttp2/nghttp2.h>

#define FRAME_HEADER_SIZE 9

typedef enum {
        STATE_NONE,
        STATE_WRITE_HEADERS,
        STATE_WRITE_DATA,
        STATE_WRITE_DONE,
        STATE_READ_HEADERS,
        STATE_READ_DATA_START,
        STATE_READ_DATA,
        STATE_READ_DONE,
} SoupHTTP2IOState;

typedef struct {
        SoupClientMessageIO iface;

        GIOStream *stream;
        GInputStream *istream;
        GOutputStream *ostream;
        guint64 connection_id;

        GMainContext *async_context;

        GHashTable *messages;

        nghttp2_session *session;

        /* Owned by nghttp2 */
        guint8 *write_buffer;
        gssize write_buffer_size;
        gssize written_bytes;

        gboolean is_shutdown;
} SoupClientMessageIOHTTP2;

typedef struct {
        SoupMessageQueueItem *item;
        SoupMessage *msg;
        SoupMessageMetrics *metrics;
        GCancellable *cancellable;
        GInputStream *decoded_data_istream;
        GInputStream *body_istream;

        /* Request body logger */
        SoupLogger *logger;

        /* Both data sources */
        GCancellable *data_source_cancellable;

        /* Pollable data sources */
        GSource *data_source_poll;

        /* Non-pollable data sources */
        GByteArray *data_source_buffer;
        GError *data_source_error;
        gboolean data_source_eof;

        GSource *io_source;
        SoupClientMessageIOHTTP2 *io; /* Unowned */
        SoupMessageIOCompletionFn completion_cb;
        gpointer completion_data;
        SoupHTTP2IOState state;
        GError *error;
        gboolean paused;
        guint32 stream_id;
} SoupHTTP2MessageData;

static gboolean io_read (SoupClientMessageIOHTTP2 *, gboolean, GCancellable *, GError **);

static void
NGCHECK (int return_code)
{
        if (return_code == NGHTTP2_ERR_NOMEM)
                g_abort ();
        else if (return_code < 0)
                g_debug ("Unhandled NGHTTP2 Error: %s", nghttp2_strerror (return_code));
}

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
        case NGHTTP2_PING:
                return "PING";
        case NGHTTP2_GOAWAY:
                return "GOAWAY";
        case NGHTTP2_WINDOW_UPDATE:
                return "WINDOW_UPDATE";
        /* LCOV_EXCL_START */
        case NGHTTP2_PUSH_PROMISE:
                return "PUSH_PROMISE";
        case NGHTTP2_CONTINUATION:
                return "CONTINUATION";
        case NGHTTP2_ALTSVC:
                return "ALTSVC";
        case NGHTTP2_ORIGIN:
                return "ORIGIN";
        default:
                g_warn_if_reached ();
                return "UNKNOWN";
        /* LCOV_EXCL_STOP */
        }
}

static const char *
state_to_string (SoupHTTP2IOState state)
{
        switch (state) {
        case STATE_NONE:
                return "NONE";
        case STATE_WRITE_HEADERS:
                return "WRITE_HEADERS";
        case STATE_WRITE_DATA:
                return "WRITE_DATA";
        case STATE_WRITE_DONE:
                return "WRITE_DONE";
        case STATE_READ_HEADERS:
                return "READ_HEADERS";
        case STATE_READ_DATA_START:
                return "READ_DATA_START";
        case STATE_READ_DATA:
                return "READ_DATA";
        case STATE_READ_DONE:
                return "READ_DONE";
        default:
                g_assert_not_reached ();
                return "";
        }
}

G_GNUC_PRINTF(3, 0)
static void
h2_debug (SoupClientMessageIOHTTP2   *io,
          SoupHTTP2MessageData       *data,
          const char                 *format,
          ...)
{
        va_list args;
        char *message;
        guint32 stream_id = 0;

        if (g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, G_LOG_DOMAIN))
                return;

	va_start (args, format);
	message = g_strdup_vprintf (format, args);
	va_end (args);

        if (data)
                stream_id = data->stream_id;

        g_assert (io);
        g_log (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "[C%" G_GUINT64_FORMAT "-S%u] [%s] %s", io->connection_id, stream_id, data ? state_to_string (data->state) : "-", message);

        g_free (message);
}

static SoupClientMessageIOHTTP2 *
get_io_data (SoupMessage *msg)
{
        return (SoupClientMessageIOHTTP2 *)soup_message_get_io_data (msg);
}

static int
get_data_io_priority (SoupHTTP2MessageData *data)
{
	if (!data->item->task)
		return G_PRIORITY_DEFAULT;

	return g_task_get_priority (data->item->task);
}

static void
set_error_for_data (SoupHTTP2MessageData *data,
                    GError               *error)
{
        h2_debug (data->io, data, "[SESSION] Error: %s", error->message);

        /* First error is probably the one we want. */
        if (!data->error)
                data->error = error;
        else
                g_error_free (error);
}

static void
advance_state_from (SoupHTTP2MessageData *data,
                    SoupHTTP2IOState      from,
                    SoupHTTP2IOState      to)
{
        if (data->state != from) {
                g_warning ("Unexpected state changed %s -> %s, expected to be from %s",
                           state_to_string (data->state), state_to_string (to),
                           state_to_string (from));
        }

        /* State never goes backwards */
        if (to < data->state) {
                g_warning ("Unexpected state changed %s -> %s, expected %s -> %s\n",
                           state_to_string (data->state), state_to_string (to),
                           state_to_string (from), state_to_string (to));
                return;
        }

        h2_debug (data->io, data, "[SESSION] State %s -> %s",
                  state_to_string (data->state), state_to_string (to));
        data->state = to;
}

/* HTTP2 read callbacks */

static int
on_header_callback (nghttp2_session     *session,
                    const nghttp2_frame *frame,
                    const uint8_t       *name,
                    size_t               namelen,
                    const uint8_t       *value,
                    size_t               valuelen,
                    uint8_t              flags,
                    void                *user_data)
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
                g_debug ("Unknown header: %s = %s", name, value);
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
memory_stream_need_more_data_callback (SoupBodyInputStreamHttp2 *stream,
                                       GCancellable             *cancellable,
                                       gboolean                  blocking,
                                       gpointer                  user_data)
{
        SoupHTTP2MessageData *data = (SoupHTTP2MessageData*)user_data;
        GError *error = NULL;

        if (!nghttp2_session_want_read (data->io->session))
                return blocking ? NULL : g_error_new_literal (G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK, _("Operation would block"));
        io_read (data->io, blocking, cancellable, &error);

        return error;
}

static int
on_begin_frame_callback (nghttp2_session        *session,
                         const nghttp2_frame_hd *hd,
                         void                   *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, hd->stream_id);

        h2_debug (user_data, data, "[RECV] [%s] Beginning", frame_type_to_string (hd->type));

        if (!data)
                return 0;

        switch (hd->type) {
        case NGHTTP2_HEADERS:
                if (data->state < STATE_READ_HEADERS)
                        advance_state_from (data, STATE_WRITE_DONE, STATE_READ_HEADERS);
                break;
        case NGHTTP2_DATA:
                if (data->state < STATE_READ_DATA_START) {
                        g_assert (!data->body_istream);
                        data->body_istream = soup_body_input_stream_http2_new (G_POLLABLE_INPUT_STREAM (data->io->istream));
                        g_signal_connect (data->body_istream, "need-more-data",
                                          G_CALLBACK (memory_stream_need_more_data_callback), data);

                        g_assert (!data->decoded_data_istream);
                        data->decoded_data_istream = soup_session_setup_message_body_input_stream (data->item->session,
                                                                                                   data->msg,
                                                                                                   data->body_istream,
                                                                                                   SOUP_STAGE_MESSAGE_BODY);

                        advance_state_from (data, STATE_READ_HEADERS, STATE_READ_DATA_START);
                }
                break;
        }

        return 0;
}

static void
handle_goaway (SoupClientMessageIOHTTP2 *io,
               guint32                   error_code,
               guint32                   last_stream_id)
{
        GHashTableIter iter;
        SoupHTTP2MessageData *data;

        g_hash_table_iter_init (&iter, io->messages);
        while (g_hash_table_iter_next (&iter, NULL, (gpointer*)&data)) {
                /* If there is no error it is a graceful shutdown and
                 * existing messages can be handled otherwise it is a fatal error */
                if ((error_code == 0 && data->stream_id > last_stream_id) ||
                     data->state < STATE_READ_DONE) {
                        /* TODO: We can restart unfinished messages */
                        set_error_for_data (data, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                            "HTTP/2 Error: %s", nghttp2_http2_strerror (error_code)));
                }
        }
}

static int
on_frame_recv_callback (nghttp2_session     *session,
                        const nghttp2_frame *frame,
                        gpointer             user_data)
{
        SoupClientMessageIOHTTP2 *io = user_data;
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
                                advance_state_from (data, STATE_READ_HEADERS, STATE_READ_DONE);
                                return 0;
                        }

                        if (soup_message_get_status (data->msg) == SOUP_STATUS_NO_CONTENT ||
                            frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                                h2_debug (io, data, "Stream done");
                                advance_state_from (data, STATE_READ_HEADERS, STATE_READ_DATA);
                        }
                        soup_message_got_headers (data->msg);
                }
                break;
        case NGHTTP2_DATA:
                if (data->metrics)
                        data->metrics->response_body_bytes_received += frame->data.hd.length + FRAME_HEADER_SIZE;
                if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM && data->body_istream)
                        soup_body_input_stream_http2_complete (SOUP_BODY_INPUT_STREAM_HTTP2 (data->body_istream));
                break;
        case NGHTTP2_RST_STREAM:
                if (frame->rst_stream.error_code != NGHTTP2_NO_ERROR) {
                        set_error_for_data (data, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                                       nghttp2_http2_strerror (frame->rst_stream.error_code)));
                }
                break;
        };

        return 0;
}

static int
on_data_chunk_recv_callback (nghttp2_session *session,
                             uint8_t          flags,
                             int32_t          stream_id,
                             const uint8_t   *data,
                             size_t           len,
                             void            *user_data)
{
        SoupClientMessageIOHTTP2 *io = user_data;
        SoupHTTP2MessageData *msgdata = nghttp2_session_get_stream_user_data (session, stream_id);

        if (!msgdata)
                return NGHTTP2_ERR_CALLBACK_FAILURE;

        h2_debug (io, msgdata, "[DATA] Recieved chunk, len=%zu, flags=%u, paused=%d", len, flags, msgdata->paused);

        if (msgdata->paused)
                return NGHTTP2_ERR_PAUSE;

        g_assert (msgdata->body_istream != NULL);
        soup_body_input_stream_http2_add_data (SOUP_BODY_INPUT_STREAM_HTTP2 (msgdata->body_istream), data, len);

        return 0;
}

/* HTTP2 write callbacks */

static int
on_before_frame_send_callback (nghttp2_session     *session,
                               const nghttp2_frame *frame,
                               void                *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);

        if (!data)
                return 0;

        switch (frame->hd.type) {
        case NGHTTP2_HEADERS:
                advance_state_from (data, STATE_NONE, STATE_WRITE_HEADERS);
                break;
        }

        return 0;
}

static int
on_frame_send_callback (nghttp2_session     *session,
                        const nghttp2_frame *frame,
                        void                *user_data)
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
                                advance_state_from (data, STATE_WRITE_HEADERS, STATE_WRITE_DONE);
                                soup_message_wrote_body (data->msg);
                        }
                }
                break;
        case NGHTTP2_DATA:
                h2_debug (user_data, data, "[SEND] [DATA] bytes=%zu, finished=%d",
                          frame->data.hd.length, frame->hd.flags & NGHTTP2_FLAG_END_STREAM);
                if (data->state < STATE_WRITE_DATA)
                        advance_state_from (data, STATE_WRITE_HEADERS, STATE_WRITE_DATA);
                if (data->metrics) {
                        data->metrics->request_body_bytes_sent += frame->hd.length + FRAME_HEADER_SIZE;
                        data->metrics->request_body_size += frame->data.hd.length;
                }
                if (frame->data.hd.length)
                        soup_message_wrote_body_data (data->msg, frame->data.hd.length);
                if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                        advance_state_from (data, STATE_WRITE_DATA, STATE_WRITE_DONE);
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
on_frame_not_send_callback (nghttp2_session     *session,
                            const nghttp2_frame *frame,
                            int                  lib_error_code,
                            void                *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, frame->hd.stream_id);

        h2_debug (user_data, data, "[SEND] [%s] Failed: %s", frame_type_to_string (frame->hd.type),
                  nghttp2_strerror (lib_error_code));

        return 0;
}

static int
on_stream_close_callback (nghttp2_session *session,
                          int32_t          stream_id,
                          uint32_t         error_code,
                          void            *user_data)
{
        g_debug ("[S%d] [SESSION] Closed: %s", stream_id, nghttp2_http2_strerror (error_code));
        return 0;
}

static gboolean
on_data_readable (GInputStream *stream,
                  gpointer      user_data)
{
        SoupHTTP2MessageData *data = (SoupHTTP2MessageData*)user_data;

        NGCHECK (nghttp2_session_resume_data (data->io->session, data->stream_id));

        g_clear_pointer (&data->data_source_poll, g_source_unref);
        return G_SOURCE_REMOVE;
}

static void
on_data_read (GInputStream *source,
              GAsyncResult *res,
              gpointer      user_data)
{
        SoupHTTP2MessageData *data = user_data;
        GError *error = NULL;
        gssize read = g_input_stream_read_finish (source, res, &error);

        h2_debug (data->io, data, "[SEND_BODY] Read %zu", read);

        /* This operation may have outlived the message data in which
           case this will have been cancelled. */
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

        h2_debug (data->io, data, "[SEND_BODY] Resuming send");
        NGCHECK (nghttp2_session_resume_data (data->io->session, data->stream_id));
}

static void
log_request_data (SoupHTTP2MessageData *data,
                  const guint8         *buffer,
                  gsize                 len)
{
        if (!data->logger)
                return;

        /* NOTE: This doesn't exactly log data as it hits the network but
           rather as soon as we read it from our source which is as good
           as we can do since nghttp handles the actual io. */
        soup_logger_log_request_data (data->logger, data->msg, (const char *)buffer, len);
}

static ssize_t
on_data_source_read_callback (nghttp2_session     *session,
                              int32_t              stream_id,
                              uint8_t             *buf,
                              size_t               length,
                              uint32_t            *data_flags,
                              nghttp2_data_source *source,
                              void                *user_data)
{
        SoupHTTP2MessageData *data = nghttp2_session_get_stream_user_data (session, stream_id);
        SoupClientMessageIOHTTP2 *io = get_io_data (data->msg);

        if (data->paused) {
                h2_debug (io, data, "[SEND_BODY] Paused");
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
                        h2_debug (io, data, "[SEND_BODY] Read %zu", read);
                        log_request_data (data, buf, read);
                }

                if (read < 0) {
                        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                                g_assert (data->data_source_poll == NULL);

                                h2_debug (io, data, "[SEND_BODY] Polling");
                                data->data_source_poll = g_pollable_input_stream_create_source (in_stream, data->data_source_cancellable);
                                g_source_set_callback (data->data_source_poll, (GSourceFunc)on_data_readable, data, NULL);
                                g_source_set_priority (data->data_source_poll, get_data_io_priority (data));
                                g_source_attach (data->data_source_poll, g_main_context_get_thread_default ());

                                g_error_free (error);
                                return NGHTTP2_ERR_DEFERRED;
                        }

                        set_error_for_data (data, g_steal_pointer (&error));
                        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
                }
                else if (read == 0) {
                        h2_debug (io, data, "[SEND_BODY] EOF");
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
                        h2_debug (io, data, "[SEND_BODY] Sending %zu", buffer_len);
                        g_assert (buffer_len <= length); /* QUESTION: Maybe not reliable */
                        memcpy (buf, data->data_source_buffer->data, buffer_len);
                        log_request_data (data, buf, buffer_len);
                        g_byte_array_set_size (data->data_source_buffer, 0);
                        return buffer_len;
                } else if (data->data_source_eof) {
                        h2_debug (io, data, "[SEND_BODY] EOF");
                        g_clear_object (&data->data_source_cancellable);
                        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
                        return 0;
                } else if (data->data_source_error) {
                        g_clear_object (&data->data_source_cancellable);
                        set_error_for_data (data, g_steal_pointer (&data->data_source_error));
                        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
                } else {
                        h2_debug (io, data, "[SEND_BODY] Reading async");
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

static SoupHTTP2MessageData *
add_message_to_io_data (SoupClientMessageIOHTTP2        *io,
                        SoupMessageQueueItem      *item,
                        SoupMessageIOCompletionFn  completion_cb,
                        gpointer                   completion_data)
{
        SoupHTTP2MessageData *data = g_new0 (SoupHTTP2MessageData, 1);

        data->item = soup_message_queue_item_ref (item);
        data->msg = item->msg;
        data->metrics = soup_message_get_metrics (data->msg);
        data->cancellable = item->cancellable;
        data->completion_cb = completion_cb;
        data->completion_data = completion_data;
        data->stream_id = 0;
        data->io = io;

        if (!g_hash_table_insert (io->messages, item->msg, data))
                g_warn_if_reached ();

        return data;
}

static void
soup_http2_message_data_free (SoupHTTP2MessageData *data)
{
        if (data->body_istream)
                g_signal_handlers_disconnect_by_data (data->body_istream, data);

        g_clear_pointer (&data->item, soup_message_queue_item_unref);
        g_clear_object (&data->body_istream);
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

        g_clear_error (&data->error);

        g_free (data);
}

static gboolean
request_header_is_valid (const char *name)
{
        static GHashTable *invalid_request_headers = NULL;

        if (g_once_init_enter (&invalid_request_headers)) {
                GHashTable *headers;

                headers= g_hash_table_new (soup_str_case_hash, soup_str_case_equal);
                g_hash_table_add (headers, "Connection");
                g_hash_table_add (headers, "Keep-Alive");
                g_hash_table_add (headers, "Proxy-Connection");
                g_hash_table_add (headers, "Transfer-Encoding");
                g_hash_table_add (headers, "Upgrade");

                g_once_init_leave (&invalid_request_headers, headers);
        }

        return !g_hash_table_contains (invalid_request_headers, name);
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
send_message_request (SoupMessage          *msg,
                      SoupClientMessageIOHTTP2   *io,
                      SoupHTTP2MessageData *data)
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
                if (!request_header_is_valid (name))
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
soup_client_message_io_http2_send_item (SoupClientMessageIO       *iface,
                                        SoupMessageQueueItem      *item,
                                        SoupMessageIOCompletionFn  completion_cb,
                                        gpointer                   user_data)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = add_message_to_io_data (io, item, completion_cb, user_data);

        send_message_request (item->msg, io, data);
}

static SoupHTTP2MessageData *
get_data_for_message (SoupClientMessageIOHTTP2 *io,
                      SoupMessage              *msg)
{
        return g_hash_table_lookup (io->messages, msg);
}

static void
soup_client_message_io_http2_finished (SoupClientMessageIO *iface,
                                       SoupMessage         *msg)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data;
	SoupMessageIOCompletionFn completion_cb;
	gpointer completion_data;
        SoupMessageIOCompletion completion;

        data = get_data_for_message (io, msg);

        completion = data->state < STATE_READ_DONE ? SOUP_MESSAGE_IO_INTERRUPTED : SOUP_MESSAGE_IO_COMPLETE;

        h2_debug (io, data, "Finished: %s", completion == SOUP_MESSAGE_IO_COMPLETE ? "completed" : "interrupted");

        // ret = nghttp2_session_terminate_session (io->session, NGHTTP2_NO_ERROR);
        // g_assert (ret == 0);

	completion_cb = data->completion_cb;
	completion_data = data->completion_data;

	g_object_ref (msg);

        NGCHECK (nghttp2_submit_rst_stream (io->session, NGHTTP2_FLAG_NONE, data->stream_id,
                                            completion == SOUP_MESSAGE_IO_COMPLETE ? NGHTTP2_NO_ERROR : NGHTTP2_CANCEL));
        nghttp2_session_set_stream_user_data (io->session, data->stream_id, NULL);
        if (!g_hash_table_remove (io->messages, msg))
                g_warn_if_reached ();

	if (completion_cb)
		completion_cb (G_OBJECT (msg), SOUP_MESSAGE_IO_COMPLETE, completion_data);

	g_object_unref (msg);
}

static void
soup_client_message_io_http2_pause (SoupClientMessageIO *iface,
                                    SoupMessage         *msg)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        h2_debug (io, data, "[SESSION] Paused");

        if (data->paused)
                g_warn_if_reached ();

        data->paused = TRUE;
}

static void
soup_client_message_io_http2_unpause (SoupClientMessageIO *iface,
                                      SoupMessage         *msg)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        h2_debug (io, data, "[SESSION] Unpaused");

        if (!data->paused)
                g_warn_if_reached ();

        data->paused = FALSE;
}

static void
soup_client_message_io_http2_stolen (SoupClientMessageIO *iface)
{
        g_assert_not_reached ();
}

static gboolean
soup_client_message_io_http2_in_progress (SoupClientMessageIO *iface,
                                          SoupMessage         *msg)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;

        return io && get_data_for_message (io, msg) != NULL;
}

static gboolean
soup_client_message_io_http2_is_paused (SoupClientMessageIO *iface,
                                        SoupMessage         *msg)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        return data->paused;
}

static gboolean
soup_client_message_io_http2_is_reusable (SoupClientMessageIO *iface)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;

        if (!nghttp2_session_want_write (io->session) && !nghttp2_session_want_read (io->session))
                return FALSE;

        return !io->is_shutdown;
}

static gboolean
message_source_check (GSource *source)
{
	SoupMessageIOSource *message_source = (SoupMessageIOSource *)source;
        SoupMessage *msg = SOUP_MESSAGE (message_source->msg);
        SoupClientMessageIOHTTP2 *io = get_io_data (msg);
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);

        /* QUESTION: What is the point of message_source->paused */

        return !data->paused;
}

static GSource *
soup_client_message_io_http2_get_source (SoupMessage             *msg,
                                         GCancellable            *cancellable,
                                         SoupMessageIOSourceFunc  callback,
                                         gpointer                 user_data)
{
        SoupClientMessageIOHTTP2 *io = get_io_data (msg);
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);
        GSource *base_source;

        /* TODO: Handle mixing writes in? */
        if (data->paused)
                base_source = cancellable ? g_cancellable_source_new (cancellable) : NULL;
        else if (data->state < STATE_WRITE_DONE && nghttp2_session_want_write (io->session))
                base_source = g_pollable_output_stream_create_source (G_POLLABLE_OUTPUT_STREAM (io->ostream), cancellable);
        else if (data->state < STATE_READ_DONE && data->decoded_data_istream)
                base_source = g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM (data->decoded_data_istream), cancellable);
        else if (data->state < STATE_READ_DONE && nghttp2_session_want_read (io->session))
                base_source = g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM (io->istream), cancellable);
        else {
                g_warn_if_reached ();
                base_source = g_timeout_source_new (0);
        }

        GSource *source = soup_message_io_source_new (base_source, G_OBJECT (msg), data->paused, message_source_check);
        g_source_set_callback (source, (GSourceFunc)callback, user_data, NULL);
        return source;
}

static void
client_stream_eof (SoupClientInputStream *stream,
                   gpointer               user_data)
{
	SoupMessage *msg = user_data;
	SoupClientMessageIOHTTP2 *io = get_io_data (msg);

        if (!io) {
                g_warn_if_reached ();
                return;
        }

        SoupHTTP2MessageData *data = get_data_for_message (io, msg);
        h2_debug (io, data, "Client stream EOF");
        advance_state_from (data, STATE_READ_DATA, STATE_READ_DONE);
        soup_message_got_body (data->msg);
}

static GInputStream *
soup_client_message_io_http2_get_response_istream (SoupClientMessageIO  *iface,
                                                   SoupMessage          *msg,
                                                   GError              **error)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);
        GInputStream *client_stream, *base_stream;

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
io_read (SoupClientMessageIOHTTP2  *io,
         gboolean             blocking,
	 GCancellable        *cancellable,
         GError             **error)
{
        guint8 buffer[8192];
        gssize read;
        int ret;

        if ((read = g_pollable_stream_read (io->istream, buffer, sizeof (buffer),
                                            blocking, cancellable, error)) < 0)
            return FALSE;

        ret = nghttp2_session_mem_recv (io->session, buffer, read);
        NGCHECK (ret);
        return ret != 0;
}

static gboolean
io_write (SoupClientMessageIOHTTP2 *io,
         gboolean                   blocking,
	 GCancellable              *cancellable,
         GError                   **error)
{
        /* We must write all of nghttp2's buffer before we ask for more */

        if (io->written_bytes == io->write_buffer_size)
                io->write_buffer = NULL;

        if (io->write_buffer == NULL) {
                io->written_bytes = 0;
                io->write_buffer_size = nghttp2_session_mem_send (io->session, (const guint8**)&io->write_buffer);
                NGCHECK (io->write_buffer_size);
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

static void
io_try_sniff_content (SoupHTTP2MessageData *data,
                      gboolean              blocking,
                      GCancellable         *cancellable)
{
        GError *error = NULL;

        if (soup_message_try_sniff_content (data->msg, data->decoded_data_istream, blocking, cancellable, &error)) {
                h2_debug (data->io, data, "[DATA] Sniffed content");
                advance_state_from (data, STATE_READ_DATA_START, STATE_READ_DATA);
        } else {
                h2_debug (data->io, data, "[DATA] Sniffer stream was not ready %s", error->message);

                g_clear_error (&error);
        }
}

static gboolean
io_run (SoupHTTP2MessageData *data,
        gboolean              blocking,
        GCancellable         *cancellable,
        GError              **error)
{
        gboolean progress = FALSE;

        if (data->state == STATE_READ_DATA_START)
                io_try_sniff_content (data, blocking, cancellable);

        if (data->state < STATE_WRITE_DONE && nghttp2_session_want_write (data->io->session))
                progress = io_write (data->io, blocking, cancellable, error);
        else if (data->state < STATE_READ_DONE && nghttp2_session_want_read (data->io->session)) {
                progress = io_read (data->io, blocking, cancellable, error);

                if (progress && data->state == STATE_READ_DATA_START)
                        io_try_sniff_content (data, blocking, cancellable);
        }

        return progress;
}

static gboolean
io_run_until (SoupClientMessageIOHTTP2 *io,
              SoupMessage              *msg,
              gboolean                  blocking,
              SoupHTTP2IOState          state,
              GCancellable             *cancellable,
              GError                  **error)
{
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

	while (progress && get_io_data (msg) == io && !data->paused && data->state < state)
                progress = io_run (data, blocking, cancellable, &my_error);

        if (my_error) {
                g_propagate_error (error, my_error);
                g_object_unref (msg);
                return FALSE;
        }

	if (data->error) {
                g_propagate_error (error, g_steal_pointer (&data->error));
		g_object_unref (msg);
		return FALSE;
        }

        if (get_io_data (msg) != io) {
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
soup_client_message_io_http2_run_until_read (SoupClientMessageIO  *iface,
                                             SoupMessage          *msg,
                                             GCancellable         *cancellable,
                                             GError              **error)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;

        return io_run_until (io, msg, TRUE, STATE_READ_DATA, cancellable, error);
}

static gboolean
soup_client_message_io_http2_skip (SoupClientMessageIO *iface,
                                   SoupMessage         *msg,
                                   gboolean             blocking,
                                   GCancellable        *cancellable,
                                   GError             **error)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;
        SoupHTTP2MessageData *data;

        if (g_cancellable_set_error_if_cancelled (cancellable, error))
                return FALSE;

        data = get_data_for_message (io, msg);
        if (!data || data->state == STATE_READ_DONE)
                return TRUE;

        h2_debug (io, data, "Skip");
        NGCHECK (nghttp2_submit_rst_stream (io->session, NGHTTP2_FLAG_NONE, data->stream_id, NGHTTP2_STREAM_CLOSED));
        return TRUE;
}

static void
soup_client_message_io_http2_run (SoupClientMessageIO *iface,
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
        SoupClientMessageIOHTTP2 *io = get_io_data (msg);
        SoupHTTP2MessageData *data = get_data_for_message (io, msg);
        GError *error = NULL;

        if (data->io_source) {
                g_source_destroy (data->io_source);
                g_clear_pointer (&data->io_source, g_source_unref);
        }

        if (io_run_until (io, msg, FALSE,
                          STATE_READ_DATA,
                          g_task_get_cancellable (task),
                          &error)) {
                g_task_return_boolean (task, TRUE);
                g_object_unref (task);
                return;
        }

        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
                g_error_free (error);
                data->io_source = soup_client_message_io_http2_get_source (msg, g_task_get_cancellable (task),
                                                                  (SoupMessageIOSourceFunc)io_run_until_read_ready,
                                                                  task);
		g_source_set_priority (data->io_source, g_task_get_priority (task));
                g_source_attach (data->io_source, io->async_context);
                return;
        }

        if (get_io_data (msg) == io)
                soup_client_message_io_http2_finished ((SoupClientMessageIO *)io, msg);
        else
                g_warn_if_reached ();

        g_task_return_error (task, error);
        g_object_unref (task);
}

static void
soup_client_message_io_http2_run_until_read_async (SoupClientMessageIO *iface,
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
soup_client_message_io_http2_is_open (SoupClientMessageIO *iface)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;

        return nghttp2_session_want_read (io->session) || nghttp2_session_want_write (io->session);
}

static void
soup_client_message_io_http2_destroy (SoupClientMessageIO *iface)
{
        SoupClientMessageIOHTTP2 *io = (SoupClientMessageIOHTTP2 *)iface;

        g_clear_object (&io->stream);
        g_clear_pointer (&io->async_context, g_main_context_unref);
        g_clear_pointer (&io->session, nghttp2_session_del);
        g_clear_pointer (&io->messages, g_hash_table_unref);

        g_free (io);
}

static const SoupClientMessageIOFuncs io_funcs = {
        soup_client_message_io_http2_destroy,
        soup_client_message_io_http2_finished,
        soup_client_message_io_http2_stolen,
        soup_client_message_io_http2_send_item,
        soup_client_message_io_http2_get_response_istream,
        soup_client_message_io_http2_pause,
        soup_client_message_io_http2_unpause,
        soup_client_message_io_http2_is_paused,
        soup_client_message_io_http2_run,
        soup_client_message_io_http2_run_until_read,
        soup_client_message_io_http2_run_until_read_async,
        soup_client_message_io_http2_skip,
        soup_client_message_io_http2_is_open,
        soup_client_message_io_http2_in_progress,
        soup_client_message_io_http2_is_reusable
};

G_GNUC_PRINTF(1, 0)
static void
debug_nghttp2 (const char *format,
               va_list     args)
{
        char *message;
        gsize len;

        if (g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, "nghttp2"))
                return;

        message = g_strdup_vprintf (format, args);
        len = strlen (message);
        if (len >= 1 && message[len - 1] == '\n')
                message[len - 1] = '\0';
        g_log ("nghttp2", G_LOG_LEVEL_DEBUG, "[NGHTTP2] %s", message);
        g_free (message);
}

static void
soup_client_message_io_http2_init (SoupClientMessageIOHTTP2 *io)
{
        static gsize nghttp2_debug_init = 0;
        if (g_once_init_enter (&nghttp2_debug_init)) {
                nghttp2_set_debug_vprintf_callback(debug_nghttp2);
                g_once_init_leave (&nghttp2_debug_init, 1);
        }

        nghttp2_session_callbacks *callbacks;
        NGCHECK (nghttp2_session_callbacks_new (&callbacks));
        nghttp2_session_callbacks_set_on_header_callback (callbacks, on_header_callback);
        nghttp2_session_callbacks_set_on_frame_recv_callback (callbacks, on_frame_recv_callback);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback (callbacks, on_data_chunk_recv_callback);
        nghttp2_session_callbacks_set_on_begin_frame_callback (callbacks, on_begin_frame_callback);
        nghttp2_session_callbacks_set_before_frame_send_callback (callbacks, on_before_frame_send_callback);
        nghttp2_session_callbacks_set_on_frame_not_send_callback (callbacks, on_frame_not_send_callback);
        nghttp2_session_callbacks_set_on_frame_send_callback (callbacks, on_frame_send_callback);
        nghttp2_session_callbacks_set_on_stream_close_callback (callbacks, on_stream_close_callback);

        NGCHECK (nghttp2_session_client_new (&io->session, callbacks, io));
        nghttp2_session_callbacks_del (callbacks);

        io->messages = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)soup_http2_message_data_free);

        io->iface.funcs = &io_funcs;
}

#define INITIAL_WINDOW_SIZE (32 * 1024 * 1024) /* 32MB matches other implementations */
#define MAX_HEADER_TABLE_SIZE 65536 /* Match size used by Chromium/Firefox */

SoupClientMessageIO *
soup_client_message_io_http2_new (GIOStream *stream, guint64 connection_id)
{
        SoupClientMessageIOHTTP2 *io = g_new0 (SoupClientMessageIOHTTP2, 1);
        soup_client_message_io_http2_init (io);

        io->stream = g_object_ref (stream);
        io->istream = g_io_stream_get_input_stream (io->stream);
        io->ostream = g_io_stream_get_output_stream (io->stream);
        io->connection_id = connection_id;

        io->async_context = g_main_context_ref_thread_default ();

        NGCHECK (nghttp2_session_set_local_window_size (io->session, NGHTTP2_FLAG_NONE, 0, INITIAL_WINDOW_SIZE));

        const nghttp2_settings_entry settings[] = {
                { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, INITIAL_WINDOW_SIZE },
                { NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, MAX_HEADER_TABLE_SIZE },
                { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 },
        };
        NGCHECK (nghttp2_submit_settings (io->session, NGHTTP2_FLAG_NONE, settings, G_N_ELEMENTS (settings)));

        return (SoupClientMessageIO *)io;
}
