/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2000-2003, Ximian, Inc.
 */

#ifndef __SOUP_H__
#define __SOUP_H__ 1

#ifdef __cplusplus
extern "C" {
#endif

#define __SOUP_H_INSIDE__

#include <libsoup/soup-address.h>
#include <libsoup/soup-auth.h>
#include <libsoup/soup-auth-domain.h>
#include <libsoup/soup-auth-domain-basic.h>
#include <libsoup/soup-auth-domain-digest.h>
#include <libsoup/soup-auth-manager.h>
#include <libsoup/soup-cache.h>
#include <libsoup/soup-content-decoder.h>
#include <libsoup/soup-content-sniffer.h>
#include <libsoup/soup-cookie.h>
#include <libsoup/soup-cookie-jar.h>
#include <libsoup/soup-cookie-jar-db.h>
#include <libsoup/soup-cookie-jar-text.h>
#include <libsoup/soup-date.h>
#include <libsoup/soup-enum-types.h>
#include <libsoup/soup-form.h>
#include <libsoup/soup-headers.h>
#include <libsoup/soup-hsts-enforcer.h>
#include <libsoup/soup-hsts-enforcer-db.h>
#include <libsoup/soup-hsts-policy.h>
#include <libsoup/soup-logger.h>
#include <libsoup/soup-message.h>
#include <libsoup/soup-method.h>
#include <libsoup/soup-multipart.h>
#include <libsoup/soup-multipart-input-stream.h>
#include <libsoup/soup-request.h>
#include <libsoup/soup-request-data.h>
#include <libsoup/soup-request-file.h>
#include <libsoup/soup-request-http.h>
#include <libsoup/soup-server.h>
#include <libsoup/soup-session-feature.h>
#include <libsoup/soup-socket.h>
#include <libsoup/soup-status.h>
#include <libsoup/soup-tld.h>
#include <libsoup/soup-uri.h>
#include <libsoup/soup-version.h>
#include <libsoup/soup-websocket.h>
#include <libsoup/soup-websocket-connection.h>
#include <libsoup/soup-websocket-extension.h>
#include <libsoup/soup-websocket-extension-deflate.h>
#include <libsoup/soup-websocket-extension-manager.h>
#include <libsoup/soup-xmlrpc.h>

#include <libsoup/soup-autocleanups.h>

#undef __SOUP_H_INSIDE__

#ifdef __cplusplus
}
#endif

#endif /* __SOUP_H__ */
