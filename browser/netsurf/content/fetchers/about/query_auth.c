/*
 * Copyright 2020 Vincent Sanders <vince@netsurf-browser.org>
 *
 * This file is part of NetSurf.
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * content generator for the about scheme query privacy page
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "utils/ns_errors.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "content/fetch.h"

#include "private.h"
#include "query_auth.h"

static char *
about_html_escape(const char *s)
{
	size_t len = 0;
	const char *p;
	char *out;
	char *q;

	if (s == NULL)
		s = "";
	for (p = s; *p != '\0'; p++) {
		switch (*p) {
		case '&': len += 5; break;
		case '<':
		case '>': len += 4; break;
		case '"': len += 6; break;
		case '\'': len += 5; break;
		default: len++; break;
		}
	}
	out = malloc(len + 1);
	if (out == NULL)
		return NULL;
	q = out;
	for (p = s; *p != '\0'; p++) {
		switch (*p) {
		case '&': memcpy(q, "&amp;", 5); q += 5; break;
		case '<': memcpy(q, "&lt;", 4); q += 4; break;
		case '>': memcpy(q, "&gt;", 4); q += 4; break;
		case '"': memcpy(q, "&quot;", 6); q += 6; break;
		case '\'': memcpy(q, "&#39;", 5); q += 5; break;
		default: *q++ = *p; break;
		}
	}
	*q = '\0';
	return out;
}

/**
 * generate the description of the login query
 */
static nserror
get_authentication_description(struct nsurl *url,
			       const char *realm,
			       const char *username,
			       const char *password,
			       char **out_str)
{
	nserror res;
	char *url_s;
	size_t url_l;
	char *str = NULL;
	const char *key;

	res = nsurl_get(url, NSURL_HOST, &url_s, &url_l);
	if (res != NSERROR_OK) {
		return res;
	}

	if ((*username == 0) && (*password == 0)) {
		key = "LoginDescription";
	} else {
		key = "LoginAgain";
	}

	str = messages_get_buff(key, url_s, realm);
	if (str != NULL) {
		NSLOG(netsurf, INFO,
		      "key:%s url:%s realm:%s str:%s",
		      key, url_s, realm, str);
		*out_str = str;
	} else {
		res = NSERROR_NOMEM;
	}

	free(url_s);

	return res;
}


/**
 * Handler to generate about scheme authentication query page
 *
 * \param ctx The fetcher context.
 * \return true if handled false if aborted.
 */
bool fetch_about_query_auth_handler(struct fetch_about_context *ctx)
{
	nserror res;
	char *url_s;
	size_t url_l;
	const char *realm = "";
	const char *username = "";
	const char *password = "";
	const char *title;
	char *description = NULL;
	char *safe_description = NULL;
	char *safe_username = NULL;
	char *safe_password = NULL;
	char *safe_url_s = NULL;
	char *safe_realm = NULL;
	struct nsurl *siteurl = NULL;
	const struct fetch_multipart_data *curmd; /* mutipart data iterator */

	/* extract parameters from multipart post data */
	curmd = fetch_about_get_multipart(ctx);
	while (curmd != NULL) {
		if (strcmp(curmd->name, "siteurl") == 0) {
			res = nsurl_create(curmd->value, &siteurl);
			if (res != NSERROR_OK) {
				return fetch_about_srverror(ctx);
			}
		} else if (strcmp(curmd->name, "realm") == 0) {
			realm = curmd->value;
		} else if (strcmp(curmd->name, "username") == 0) {
			username = curmd->value;
		} else if (strcmp(curmd->name, "password") == 0) {
			password = curmd->value;
		}
		curmd = curmd->next;
	}

	if (siteurl == NULL) {
		return fetch_about_srverror(ctx);
	}

	/* content is going to return ok */
	fetch_about_set_http_code(ctx, 200);

	/* content type */
	if (fetch_about_send_header(ctx, "Content-Type: text/html; charset=utf-8")) {
		goto fetch_about_query_auth_handler_aborted;
	}

	title = messages_get("LoginTitle");
	res = fetch_about_ssenddataf(ctx,
			"<html>\n<head>\n"
			"<title>%s</title>\n"
			"<link rel=\"stylesheet\" type=\"text/css\" "
			"href=\"resource:internal.css\">\n"
			"</head>\n"
			"<body class=\"ns-even-bg ns-even-fg ns-border\" id =\"authentication\">\n"
			"<h1 class=\"ns-border\">%s</h1>\n",
			title, title);
	if (res != NSERROR_OK) {
		goto fetch_about_query_auth_handler_aborted;
	}

	res = fetch_about_ssenddataf(ctx,
			 "<form method=\"post\""
			 " enctype=\"multipart/form-data\">");
	if (res != NSERROR_OK) {
		goto fetch_about_query_auth_handler_aborted;
	}

	res = get_authentication_description(siteurl,
					     realm,
					     username,
					     password,
					     &description);
	if (res == NSERROR_OK) {
		safe_description = about_html_escape(description);
		free(description);
		description = NULL;
		if (safe_description == NULL) {
			res = NSERROR_NOMEM;
			goto fetch_about_query_auth_handler_aborted;
		}
		res = fetch_about_ssenddataf(ctx, "<p>%s</p>", safe_description);
		free(safe_description);
		safe_description = NULL;
		if (res != NSERROR_OK) {
			goto fetch_about_query_auth_handler_aborted;
		}
	}

	res = fetch_about_ssenddataf(ctx, "<table>");
	if (res != NSERROR_OK) {
		goto fetch_about_query_auth_handler_aborted;
	}

	safe_username = about_html_escape(username);
	if (safe_username == NULL) {
		res = NSERROR_NOMEM;
		goto fetch_about_query_auth_handler_aborted;
	}
	res = fetch_about_ssenddataf(ctx,
			 "<tr>"
			 "<th><label for=\"name\">%s:</label></th>"
			 "<td><input type=\"text\" id=\"username\" "
			 "name=\"username\" value=\"%s\"></td>"
			 "</tr>",
			 messages_get("Username"), safe_username);
	free(safe_username);
	safe_username = NULL;
	if (res != NSERROR_OK) {
		goto fetch_about_query_auth_handler_aborted;
	}

	safe_password = about_html_escape(password);
	if (safe_password == NULL) {
		res = NSERROR_NOMEM;
		goto fetch_about_query_auth_handler_aborted;
	}
	res = fetch_about_ssenddataf(ctx,
			 "<tr>"
			 "<th><label for=\"password\">%s:</label></th>"
			 "<td><input type=\"password\" id=\"password\" "
			 "name=\"password\" value=\"%s\"></td>"
			 "</tr>",
			 messages_get("Password"), safe_password);
	free(safe_password);
	safe_password = NULL;
	if (res != NSERROR_OK) {
		goto fetch_about_query_auth_handler_aborted;
	}

	res = fetch_about_ssenddataf(ctx, "</table>");
	if (res != NSERROR_OK) {
		goto fetch_about_query_auth_handler_aborted;
	}

	res = fetch_about_ssenddataf(ctx,
			 "<div id=\"buttons\">"
			 "<input type=\"submit\" id=\"login\" name=\"login\" "
			 "value=\"%s\" class=\"default-action\">"
			 "<input type=\"submit\" id=\"cancel\" name=\"cancel\" "
			 "value=\"%s\">"
			 "</div>",
			 messages_get("Login"),
			 messages_get("Cancel"));
	if (res != NSERROR_OK) {
		goto fetch_about_query_auth_handler_aborted;
	}

	res = nsurl_get(siteurl, NSURL_COMPLETE, &url_s, &url_l);
	if (res != NSERROR_OK) {
		url_s = strdup("");
	}
	safe_url_s = about_html_escape(url_s);
	if (safe_url_s == NULL) {
		free(url_s);
		res = NSERROR_NOMEM;
		goto fetch_about_query_auth_handler_aborted;
	}
	res = fetch_about_ssenddataf(ctx,
			 "<input type=\"hidden\" name=\"siteurl\" value=\"%s\">",
			 safe_url_s);
	free(url_s);
	free(safe_url_s);
	safe_url_s = NULL;
	if (res != NSERROR_OK) {
		goto fetch_about_query_auth_handler_aborted;
	}

	safe_realm = about_html_escape(realm);
	if (safe_realm == NULL) {
		res = NSERROR_NOMEM;
		goto fetch_about_query_auth_handler_aborted;
	}
	res = fetch_about_ssenddataf(ctx,
			 "<input type=\"hidden\" name=\"realm\" value=\"%s\">",
			 safe_realm);
	free(safe_realm);
	safe_realm = NULL;
	if (res != NSERROR_OK) {
		goto fetch_about_query_auth_handler_aborted;
	}

	res = fetch_about_ssenddataf(ctx, "</form></body>\n</html>\n");
	if (res != NSERROR_OK) {
		goto fetch_about_query_auth_handler_aborted;
	}

	fetch_about_send_finished(ctx);

	nsurl_unref(siteurl);

	return true;

fetch_about_query_auth_handler_aborted:

	free(description);
	free(safe_description);
	free(safe_username);
	free(safe_password);
	free(safe_url_s);
	free(safe_realm);
	nsurl_unref(siteurl);

	return false;
}
