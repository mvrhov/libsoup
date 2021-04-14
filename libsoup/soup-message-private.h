/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2000-2003, Ximian, Inc.
 */

#ifndef __SOUP_MESSAGE_PRIVATE_H__
#define __SOUP_MESSAGE_PRIVATE_H__ 1

#include "soup-filter-input-stream.h"
#include "soup-message.h"
#include "soup-message-io-backend.h"
#include "soup-message-io-data.h"
#include "auth/soup-auth.h"
#include "soup-content-processor.h"
#include "content-sniffer/soup-content-sniffer.h"
#include "soup-session.h"

typedef struct _SoupClientMessageIOData SoupClientMessageIOData;
void soup_client_message_io_data_free (SoupClientMessageIOData *io);

void             soup_message_set_status       (SoupMessage      *msg,
						guint             status_code,
						const char       *reason_phrase);
void             soup_message_cleanup_response (SoupMessage      *msg);

typedef void     (*SoupMessageGetHeadersFn)  (SoupMessage      *msg,
					      GString          *headers,
					      SoupEncoding     *encoding,
					      gpointer          user_data);
typedef guint    (*SoupMessageParseHeadersFn)(SoupMessage      *msg,
					      char             *headers,
					      guint             header_len,
					      SoupEncoding     *encoding,
					      gpointer          user_data,
					      GError          **error);

/* Auth handling */
void           soup_message_set_auth       (SoupMessage *msg,
					    SoupAuth    *auth);
SoupAuth      *soup_message_get_auth       (SoupMessage *msg);
void           soup_message_set_proxy_auth (SoupMessage *msg,
					    SoupAuth    *auth);
SoupAuth      *soup_message_get_proxy_auth (SoupMessage *msg);
GUri          *soup_message_get_uri_for_auth (SoupMessage *msg);

void soup_message_wrote_headers     (SoupMessage *msg);
void soup_message_wrote_body_data   (SoupMessage *msg,
				     gsize        chunk_size);
void soup_message_wrote_body        (SoupMessage *msg);
void soup_message_got_informational (SoupMessage *msg);
void soup_message_got_headers       (SoupMessage *msg);
void soup_message_got_body          (SoupMessage *msg);
void soup_message_content_sniffed   (SoupMessage *msg,
				     const char  *content_type,
				     GHashTable  *params);
void soup_message_starting          (SoupMessage *msg);
void soup_message_restarted         (SoupMessage *msg);
void soup_message_finished          (SoupMessage *msg);
gboolean soup_message_authenticate  (SoupMessage *msg,
				     SoupAuth    *auth,
				     gboolean     retrying);
void soup_message_hsts_enforced     (SoupMessage *msg);

gboolean soup_message_disables_feature (SoupMessage *msg,
					gpointer     feature);

GList *soup_message_get_disabled_features (SoupMessage *msg);

GInputStream *soup_message_setup_body_istream (GInputStream *body_stream,
					       SoupMessage *msg,
					       SoupSession *session,
					       SoupProcessingStage start_at_stage);

SoupConnection *soup_message_get_connection (SoupMessage    *msg);
void            soup_message_set_connection (SoupMessage    *msg,
					     SoupConnection *conn);

SoupMessageIOBackend    *soup_message_get_io_data (SoupMessage             *msg);
void                     soup_message_set_io_data (SoupMessage             *msg,
						   SoupMessageIOBackend    *io);
void                     soup_message_clear_io_data (SoupMessage           *msg);

SoupContentSniffer *soup_message_get_content_sniffer    (SoupMessage        *msg);
void                soup_message_set_content_sniffer    (SoupMessage        *msg,
							 SoupContentSniffer *sniffer);
void                soup_message_set_bytes_for_sniffing (SoupMessage        *msg,
							 gsize               bytes);

GInputStream       *soup_message_get_request_body_stream (SoupMessage        *msg);

void                soup_message_set_reason_phrase       (SoupMessage        *msg,
                                                          const char         *reason_phrase);

void                soup_message_set_method              (SoupMessage        *msg,
                                                          const char         *method);

void                soup_message_set_http_version        (SoupMessage       *msg,
						          SoupHTTPVersion    version);

typedef enum {
        SOUP_MESSAGE_METRICS_FETCH_START,
        SOUP_MESSAGE_METRICS_DNS_START,
        SOUP_MESSAGE_METRICS_DNS_END,
        SOUP_MESSAGE_METRICS_CONNECT_START,
        SOUP_MESSAGE_METRICS_CONNECT_END,
        SOUP_MESSAGE_METRICS_TLS_START,
        SOUP_MESSAGE_METRICS_REQUEST_START,
        SOUP_MESSAGE_METRICS_RESPONSE_START,
        SOUP_MESSAGE_METRICS_RESPONSE_END
} SoupMessageMetricsType;

void soup_message_set_metrics_timestamp (SoupMessage           *msg,
                                         SoupMessageMetricsType type);

#endif /* __SOUP_MESSAGE_PRIVATE_H__ */
