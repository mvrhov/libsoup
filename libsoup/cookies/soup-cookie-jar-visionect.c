/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/*
 * soup-cookie-jar-visionect.c: visionect database-based cookie storage (additional hook functions for text)
 *
 * Copyright (C) 2007, 2008 Red Hat, Inc.
 * Copyright (C) 2021-2023 Visionect d.o.o.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <endian.h>

#include <libpq-fe.h>

#include "soup-cookie-jar-visionect.h"
#include "soup-cookie-jar.h"
#include "soup.h"

gboolean debugMode = false;

gboolean
is_db_mode(char *uuid);

int
soup_cookie_jar_visionect_try_connect(SoupCookieJarTextPrivate *jar);

int
rt_soup_cookie_jar_visionect_try_connect(SoupCookieJarTextPrivate *jar);

static int
migrate_and_load_old (SoupCookieJar *jar);

static void
rt_migrate_and_load_old (SoupCookieJar *jar);

static int
psql_write_cookie(PGconn *conn, char *uuid, SoupCookie *cookie);

static int
rt_psql_write_cookie(PGconn *conn, char *uuid, SoupCookie *cookie);

static int
psql_delete_cookie (PGconn *conn, char *uuid, SoupCookie *cookie);

static int
rt_psql_delete_cookie (PGconn *conn, char *uuid, SoupCookie *cookie);


static int
psql_load_cookies (SoupCookieJar *jar);

static int
rt_psql_load_cookies (SoupCookieJar *jar);

static void sleep_ms(int ms) {
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000;
	nanosleep(&ts, NULL);
}

static void vlog(char *uuid, char *fmt, ...) {
	va_list args;
	char *newFmt;
	char *ts = g_malloc(32);
	time_t t = time(NULL);

	strftime(ts, 32, "%Y-%m-%dT%H:%M:%S%z", gmtime(&t));
	newFmt = g_strdup_printf("%s INFO %s %s\n", ts, uuid, fmt);

	va_start(args, fmt);
	vfprintf(stderr, newFmt, args);
	va_end(args);

	g_free(newFmt);
	g_free(ts);
}

static char *dump_cookie(SoupCookie *cookie) {
	char *ts = g_malloc(32);
	char *ret;
	time_t t;

	t = g_date_time_to_unix (soup_cookie_get_expires (cookie));
	strftime(ts, 32, "%Y-%m-%dT%H:%M:%S%z", gmtime(&t));

	ret = g_strdup_printf ("HttpOnly:%s\tDomain:%s\tPath:%s\tSecure:%s\tExpires:%s\tExpiresTS:%ld\tName:%s\tValue:%s\tSameSite:%s",
		soup_cookie_get_http_only (cookie) ? "t" : "f",
		soup_cookie_get_domain (cookie),
		soup_cookie_get_path (cookie),
		soup_cookie_get_secure (cookie) ? "t" : "f",
		 ts, (gulong)t,
		soup_cookie_get_name (cookie),
		soup_cookie_get_value (cookie),
		same_site_policy_to_string (soup_cookie_get_same_site_policy (cookie)));

	g_free(ts);

	return ret;
}

static void debug(char *uuid, SoupCookie *cookie, char *fmt, ...) {
	va_list args;
	char *c = NULL;
	char *newFmt;
	char *ts = g_malloc(32);
	time_t t = time(NULL);

	if (!debugMode) {
		return;
	}

	if (cookie) {
		c = dump_cookie(cookie);
	}

	strftime(ts, 32, "%Y-%m-%dT%H:%M:%S%z", gmtime(&t));
	newFmt = g_strdup_printf("%s DEBUG %s %s %s %s\n", ts, uuid, fmt, c ? "COOKIE: " : "", c ? c : "");

	va_start(args, fmt);
	vfprintf(stderr, newFmt, args);
	va_end(args);

	g_free(newFmt);
	g_free(ts);
	if (c) {
		g_free(c);
	}
}

void
soup_cookie_jar_visionect_finalize (GObject *object)
{
	SoupCookieJarTextPrivate *priv =
		soup_cookie_jar_text_get_instance_private (SOUP_COOKIE_JAR_TEXT (object));

	if (priv->conn != NULL) {
		PQfinish(priv->conn);
		priv->conn = NULL;
	}
	if (priv->uuid != NULL) {
		g_free (priv->uuid);
		priv->uuid = NULL;
	}
}

gboolean
is_db_mode(char *uuid)
{
	char *psql_host;
	char *psql_port;
	char *psql_user;
	char *psql_pass;
	char *psql_db;

	psql_host = getenv("VSS_COOKIE_DB_ADDR");
	psql_port = getenv("VSS_COOKIE_DB_PORT");
	psql_user = getenv("VSS_COOKIE_DB_USER");
	psql_pass = getenv("VSS_COOKIE_DB_PASS");
	psql_db   = getenv("VSS_COOKIE_DB_NAME");

	if (psql_host == NULL || psql_port == NULL || psql_user == NULL || psql_pass == NULL || psql_db == NULL) {
		vlog(uuid, "Not all required env variables set: " \
		"VSS_COOKIE_DB_ADDR, VSS_COOKIE_DB_PORT, " \
		"VSS_COOKIE_DB_USER, VSS_COOKIE_DB_PASS, VSS_COOKIE_DB_NAME "\
		"Cookie storage configured as text files.");

		return false;
	}

	vlog(uuid, "Cookie storage configured for postgresql.");
	return true;
}

int
soup_cookie_jar_visionect_load (SoupCookieJar *jar)
{
	SoupCookieJarTextPrivate *priv =
		soup_cookie_jar_text_get_instance_private (SOUP_COOKIE_JAR_TEXT (jar));

	int len;
	char *s;

	priv->db_mode = false;
	priv->uuid = NULL;
	priv->conn = NULL;

	s = getenv("VSS_COOKIE_DEBUG");
	if (s != NULL) {
		debugMode = true;
	}

	// get the uuid from file name.. first try to remove the path
	s = g_strrstr(priv->filename, "/");
	if (s != NULL) {
		s++; // move one byte forward to also remove /
	} else {
		s = priv->filename; // no / in path just set the beginning to the start of string
	}

	// allocate space for the uuid (we allocate a bit more, but is not a lot, so it won't hurt us)
	len = strlen(s) + 1;
	priv->uuid = g_malloc(len);
	priv->uuid[len-1] = 0;
	g_strlcpy(priv->uuid, s, len);

	s = strstr(priv->uuid, "-cookies.txt");
	if (s == NULL) {
		vlog("", "skipping db mode, cookie file is named %s", priv->filename);
		return -1;
	}
	*s = 0; // remove suffix

	if (strlen(priv->uuid) != 36) {
		vlog("", "skipping db mode, invalid uuid %s len %d, cookie file is named %s", priv->uuid, strlen(priv->uuid), priv->filename);
		return -1;
	}

	if (!is_db_mode(priv->uuid)) {
		return -1;
	}

	//we set db mode only if cookie name matches
	priv->db_mode = true;

	rt_migrate_and_load_old(jar);

	if (rt_soup_cookie_jar_visionect_try_connect(priv) != 0) {
		return -1;
	}
	rt_psql_load_cookies(jar);

	return 0;
}

int
soup_cookie_jar_visionect_try_connect(SoupCookieJarTextPrivate *jar)
{
	char *psql_host;
	char *psql_port;
	char *psql_user;
	char *psql_pass;
	char *psql_db;

	gboolean connect;

	char *q;
	PGresult *res;

	connect = (jar->conn == NULL);

	if (!connect) {
		if (PQstatus(jar->conn) != CONNECTION_OK) {
			// we could have used PQreset here, but that means we have to handle yet another case..
			// so we just drop everything and continue from the blank slate
			PQfinish(jar->conn);
			connect = true;
		} else {
			// verfy connection by running a simple sql
			q = "SELECT 1;";
			res = PQexec(jar->conn, q);

			if (PQresultStatus(res) != PGRES_TUPLES_OK) {
				PQfinish(jar->conn);
				connect = true;
			}

			PQclear(res);
		}
	}

	if (connect) {
		psql_host = getenv("VSS_COOKIE_DB_ADDR");
		psql_port = getenv("VSS_COOKIE_DB_PORT");
		psql_user = getenv("VSS_COOKIE_DB_USER");
		psql_pass = getenv("VSS_COOKIE_DB_PASS");
		psql_db   = getenv("VSS_COOKIE_DB_NAME");

		if (psql_host == NULL || psql_port == NULL || psql_user == NULL || psql_pass == NULL || psql_db == NULL) {
			vlog(jar->uuid, "You must set all listed env variables: " \
			"VSS_COOKIE_DB_ADDR, VSS_COOKIE_DB_PORT, " \
			"VSS_COOKIE_DB_USER, VSS_COOKIE_DB_PASS, VSS_COOKIE_DB_NAME");
		}

		q = g_strdup_printf("host=%s port=%s user=%s password=%s dbname=%s application_name=vss_cookie_jar", psql_host, psql_port, psql_user, psql_pass, psql_db);
		jar->conn = PQconnectdb(q);
		if (PQstatus(jar->conn) != CONNECTION_OK) {
			vlog(jar->uuid, "Connection to database failed: %s",
				PQerrorMessage(jar->conn));

			PQfinish(jar->conn);
			jar->conn = NULL;
			g_free(q);

			return -1;
		}

		g_free(q);
	}

	return 0;
}

int
rt_soup_cookie_jar_visionect_try_connect(SoupCookieJarTextPrivate *jar) {
	SoupCookieJarTextPrivate *priv =
		soup_cookie_jar_text_get_instance_private (SOUP_COOKIE_JAR_TEXT (jar));

	if (soup_cookie_jar_visionect_try_connect(jar) != 0) {
		sleep_ms(500);
		vlog(priv->uuid, "retrying soup_cookie_jar_visionect_try_connect");
		return soup_cookie_jar_visionect_try_connect(jar);
	}

	return 0;
}

static SoupCookie*
parse_line (SoupCookieJar *jar, char *line, time_t now)
{
	SoupCookie *cookie;

	cookie = parse_cookie (line, now);
	if (cookie) {
		soup_cookie_jar_add_cookie (jar, cookie);
	}

	return cookie;
}

static char
*psql_get_old_cookies(PGconn *conn, char *uuid)
{
	char *q;
	char *val;
	unsigned char *val_ok;
	PGresult *res;
	size_t to_len;

	debug(uuid, NULL, "Fetching old cookies");
	q = g_strdup_printf("SELECT data FROM filestorage WHERE name = '%s-cookies.txt' AND uuid = '%s' AND type = 'session';", uuid, uuid);
	res = PQexec(conn, q);

	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		vlog(uuid, "Selecting old cookies failed: %s",
			PQresultErrorMessage(res));

		PQclear(res);
		g_free(q);
		return NULL;
	}

	// no rows.. either already migrated or no records
	if (PQntuples(res) == 0) {
		PQclear(res);
		g_free(q);
		return NULL;
	}

	val = PQgetvalue(res, 0, 0);
	val_ok = PQunescapeBytea((const unsigned char *)val, &to_len);
	PQclear(res);
	g_free(q);

	debug(uuid, NULL, "Old cookies: %s", (char *)val_ok);
	return (char *)val_ok;
}

static int
psql_delete_old_cookies(PGconn *conn, char *uuid)
{
	char *q;
	PGresult *res;

	debug(uuid, NULL, "Deleting old cookies, if there are any.");
	q = g_strdup_printf("DELETE FROM filestorage WHERE name = '%s-cookies.txt' AND uuid = '%s' AND type = 'session';", uuid, uuid);
	res = PQexec(conn, q);

	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		vlog(uuid, "Deleting old cookies failed: %s",
			PQresultErrorMessage(res));

		PQclear(res);
		g_free(q);
		return -1;
	}

	PQclear(res);
	g_free(q);

	return 0;
}

static int
rt_psql_delete_old_cookies(PGconn *conn, char *uuid)
{
	if (psql_delete_old_cookies(conn, uuid) != 0) {
		sleep_ms(500);
		vlog(uuid, "retrying psql_delete_old_cookies");
		return psql_delete_old_cookies(conn, uuid);
	}

	return 0;
}

static int
migrate_and_load_old (SoupCookieJar *jar)
{
	SoupCookieJarTextPrivate *priv =
		soup_cookie_jar_text_get_instance_private (SOUP_COOKIE_JAR_TEXT (jar));

	char *contents = NULL, *line, *p;
	time_t now = time (NULL);
	SoupCookie *cookie;

	if (soup_cookie_jar_visionect_try_connect(priv) != 0) {
		return -1;
	}
	contents = psql_get_old_cookies(priv->conn, priv->uuid);

	// no old data.. we are done
	if (contents == NULL) {
		return 0;
	}

	line = contents;
	for (p = contents; *p; p++) {
		/* \r\n comes out as an extra empty line and gets ignored */
		if (*p == '\r' || *p == '\n') {
			*p = '\0';
			cookie = parse_line (jar, line, now);
			if (cookie) {
				rt_psql_write_cookie(priv->conn, priv->uuid, cookie);
			}
			line = p + 1;
		}
	}
	cookie = parse_line (jar, line, now);
	if (cookie) {
		rt_psql_write_cookie(priv->conn, priv->uuid, cookie);
	}

	g_free (contents);

	rt_psql_delete_old_cookies(priv->conn, priv->uuid);

	return 0;
}

static void
rt_migrate_and_load_old (SoupCookieJar *jar)
{
	SoupCookieJarTextPrivate *priv =
		soup_cookie_jar_text_get_instance_private (SOUP_COOKIE_JAR_TEXT (jar));

	if (migrate_and_load_old(jar) != 0) {
		sleep_ms(500);
		vlog(priv->uuid, "retrying migrate_and_load_old");
		migrate_and_load_old(jar);
	}
}

void
soup_cookie_jar_visionect_changed (SoupCookieJar *jar,
				  SoupCookie	*old_cookie,
				  SoupCookie	*new_cookie)
{
	SoupCookieJarTextPrivate *priv =
		soup_cookie_jar_text_get_instance_private (SOUP_COOKIE_JAR_TEXT (jar));

	// delete only when we don't have new cookie to write
	if (old_cookie && !new_cookie) {
		if (rt_soup_cookie_jar_visionect_try_connect(priv) != 0) {
			return;
		}

		rt_psql_delete_cookie (priv->conn, priv->uuid, old_cookie);
	}

	if (new_cookie) {
		if (soup_cookie_get_expires (new_cookie)) {
			if (rt_soup_cookie_jar_visionect_try_connect(priv) != 0) {
				return;
			}

			rt_psql_write_cookie(priv->conn, priv->uuid, new_cookie);
		}
	}
}

static int
psql_write_cookie(PGconn *conn, char *uuid, SoupCookie *cookie)
{
	const int PARAM_COUNT = 10;
	PGresult *res;
	const char *paramValues[PARAM_COUNT];
	char *rslt;
	gulong expires;

	debug(uuid, cookie, "Writing cookie.");

	if (soup_cookie_get_expires (cookie)) {
		expires = (gulong)g_date_time_to_unix (soup_cookie_get_expires (cookie));
		rslt = g_strdup_printf("%ld", expires);
	} else {
		rslt = NULL;
	}

	paramValues[0] = uuid;
	paramValues[1] = soup_cookie_get_domain (cookie);
	paramValues[2] = soup_cookie_get_name (cookie);
	paramValues[3] = soup_cookie_get_value (cookie);
	paramValues[4] = soup_cookie_get_path (cookie);
	paramValues[5] = rslt;
	paramValues[6] = NULL;
	paramValues[7] = g_strdup_printf("%d", soup_cookie_get_secure (cookie));
	paramValues[8] = g_strdup_printf("%d", soup_cookie_get_http_only (cookie));
	paramValues[9] = g_strdup_printf("%d", soup_cookie_get_same_site_policy (cookie));
	res = PQexecParams(conn,
			"INSERT INTO cookies (uuid, host, name, value, path, expiry, last_accessed, is_secure, is_http_only, same_site) VALUES " \
			"($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) " \
			"ON CONFLICT(uuid, host, name, path) " \
			"DO UPDATE SET value = $4, path = $5, expiry = $6, last_accessed = $7, is_secure = $8, is_http_only = $9, same_site = $10;",
 			PARAM_COUNT, NULL, paramValues, NULL, NULL, 1);

	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		vlog(uuid, "Cookie insert failed: %s",
			PQresultErrorMessage(res));

		g_free((void *)paramValues[5]);
		g_free((void *)paramValues[7]);
		g_free((void *)paramValues[8]);
		PQclear(res);
		return -1;
	}

	g_free((void *)paramValues[5]);
	g_free((void *)paramValues[7]);
	g_free((void *)paramValues[8]);
	PQclear(res);

	return 0;
}

int
rt_psql_write_cookie(PGconn *conn, char *uuid, SoupCookie *cookie)
{
	if (psql_write_cookie(conn, uuid, cookie) != 0) {
		sleep_ms(500);
		vlog(uuid, "retrying psql_write_cookie");
		return psql_write_cookie(conn, uuid, cookie);
	}

	return 0;
}

static int
psql_delete_cookie (PGconn *conn, char *uuid, SoupCookie *cookie)
{
	const int PARAM_COUNT = 4;
	PGresult *res;
	const char *paramValues[PARAM_COUNT];

	debug(uuid, cookie, "Deleting cookie.");

	paramValues[0] = uuid;
	paramValues[1] = soup_cookie_get_domain (cookie);
	paramValues[2] = soup_cookie_get_name (cookie);
	paramValues[3] = soup_cookie_get_path (cookie);
	res = PQexecParams(conn, "DELETE FROM cookies WHERE uuid = $1 AND host = $2 AND name = $3 AND path = $4;", PARAM_COUNT, NULL, paramValues, NULL, NULL, 1);

	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		vlog(uuid, "Deleting cookie failed: %s",
			PQresultErrorMessage(res));

		PQclear(res);
		return -1;
	}

	PQclear(res);

	return 0;
}

int
rt_psql_delete_cookie (PGconn *conn, char *uuid, SoupCookie *cookie)
{
	if (psql_delete_cookie(conn, uuid, cookie) != 0) {
		sleep_ms(500);
		vlog(uuid, "retrying psql_delete_cookie");
		return psql_delete_cookie(conn, uuid, cookie);
	}

	return 0;
}

static int
psql_load_cookies(SoupCookieJar *jar)
{
	SoupCookieJarTextPrivate *priv =
		soup_cookie_jar_text_get_instance_private (SOUP_COOKIE_JAR_TEXT (jar));

	const int PARAM_COUNT = 1;
	PGresult *res;
	const char *paramValues[PARAM_COUNT];
	int i;

	SoupCookie *cookie = NULL;
	char *host, *name, *value, *path;
	char *tmpStr;
	int  tmpInt;
	gulong expire_time;
	int max_age;
	gboolean secure, http_only;
	time_t now;

	paramValues[0] = priv->uuid;
	res = PQexecParams(priv->conn,
		"SELECT host, name, value, path, expiry, last_accessed, is_secure, is_http_only, same_site FROM cookies WHERE uuid = $1;",
		PARAM_COUNT, NULL, paramValues, NULL, NULL, 1);

	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		vlog(priv->uuid, "Selecting cookies failed: %s",
			PQresultErrorMessage(res));

		PQclear(res);
		return -1;
	}

	// no rows..
	if (PQntuples(res) == 0) {
		PQclear(res);
		return 0;
	}

	for (i = 0; i < PQntuples(res); i++) {
		host = PQgetvalue(res, i, 0);
		name = PQgetvalue(res, i, 1);
		value = PQgetvalue(res, i, 2);
		path = PQgetvalue(res, i, 3);

		tmpStr = PQgetvalue(res, i, 4);
		expire_time = 0;
		max_age = 0;
		if (tmpStr != NULL) {
			expire_time = be64toh(*((gulong *) tmpStr));
			now = time (NULL);
			if (now >= expire_time) {
				continue;
			}
			max_age = (expire_time - now <= G_MAXINT ? expire_time - now : G_MAXINT);
		}
		// last_accessed
		tmpStr = PQgetvalue(res, i, 5);
		/*if (tmpStr != NULL) {
			last_accessed = ntoh64(*((gulong *) tmpStr));
		}*/
		tmpStr = PQgetvalue(res, i, 6);
		secure = FALSE;
		if (tmpStr != NULL) {
			tmpInt = be32toh(*((guint32 *) tmpStr));
			secure = (tmpInt != FALSE);
		}
		tmpStr = PQgetvalue(res, i, 7);
		http_only = FALSE;
		if (tmpStr != NULL) {
			tmpInt = be32toh(*((guint32 *) tmpStr));
			http_only = (tmpInt != FALSE);
		}
		// same_site
		tmpStr = PQgetvalue(res, i, 8);

		// build up cooke and add it to jar
		cookie = soup_cookie_new (name, value, host, path, max_age);
		if (secure) {
			soup_cookie_set_secure (cookie, TRUE);
		}
		if (http_only) {
			soup_cookie_set_http_only (cookie, TRUE);
		}

		debug(priv->uuid, cookie, "Loading cookie.");
		soup_cookie_jar_add_cookie (jar, cookie);
	}
	PQclear(res);

	return 0;
}

static int
rt_psql_load_cookies (SoupCookieJar *jar)
{
	SoupCookieJarTextPrivate *priv =
		soup_cookie_jar_text_get_instance_private (SOUP_COOKIE_JAR_TEXT (jar));

	if (psql_load_cookies(jar) != 0) {
		sleep_ms(500);
		vlog(priv->uuid, "retrying psql_load_cookies");
		return psql_load_cookies(jar);
	}

	return 0;
}
