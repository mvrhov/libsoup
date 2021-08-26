/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2008 Red Hat, Inc.
 */

#pragma once

#include "soup-cookie-jar.h"

G_BEGIN_DECLS

SOUP_AVAILABLE_IN_ALL
SoupCookieJar *soup_cookie_jar_text_new (const char *filename,
					 gboolean    read_only);

G_END_DECLS

