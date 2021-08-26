/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/*
 * soup-cookie-jar-visionect.c: visionect database-based cookie storage (additional hook functions for text)
 *
 * Copyright (C) 2007, 2008 Red Hat, Inc.
 * Copyright (C) 2021-2023 Visionect d.o.o.
 */

#pragma once

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>

#include <libpq-fe.h>
#include "soup-cookie-jar.h"
#include "soup.h"

G_BEGIN_DECLS

struct _SoupCookieJarText {
	SoupCookieJar parent;

};

typedef struct {
	char *filename;

	char     *uuid;
	gboolean db_mode;
	PGconn   *conn;	
} SoupCookieJarTextPrivate;


#define SOUP_TYPE_COOKIE_JAR_TEXT (soup_cookie_jar_text_get_type ())
SOUP_AVAILABLE_IN_ALL
G_DECLARE_FINAL_TYPE (SoupCookieJarText, soup_cookie_jar_text, SOUP, COOKIE_JAR_TEXT, SoupCookieJar)


void
soup_cookie_jar_visionect_finalize (GObject *object);

int
soup_cookie_jar_visionect_load (SoupCookieJar *jar);

void
soup_cookie_jar_visionect_changed (SoupCookieJar *jar,
				  SoupCookie    *old_cookie,
				  SoupCookie    *new_cookie);

// forward declarations for functions implemented in soup_cookie_jar_text
SoupCookie*
parse_cookie (char *line, time_t now);

const char *
same_site_policy_to_string (SoupSameSitePolicy policy);

// end of forward declarations for functions implemented in soup_cookie_jar_text
G_END_DECLS