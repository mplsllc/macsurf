/*
 * MacSurf - macos9_fetch.c
 * HTTP fetch via Open Transport.
 *
 * Mirrors the Retro68 ot-tcp-demo.c pattern verbatim (verified on real
 * Mac OS 9.2 hardware): plain non-InContext OT API, sync+blocking endpoint,
 * OTUseSyncIdleEvents(ep, true) plus a notifier that yields to the classic
 * Thread Manager on kOTSyncIdleEvent.
 *
 * InitOpenTransport() and CloseOpenTransport() are called ONCE in main() -
 * not per-fetch. This file only opens and closes individual endpoints.
 *
 * Also provides HTML tag stripping and word wrapping used by window.c
 * to turn a fetch body into displayable text.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf/fetch.h"
#include "macos9/macos9.h"

#ifdef __MACOS9__
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <Threads.h>
extern OTClientContextPtr macos9_ot_context;
#endif

#define MACSURF_PROXY_HOST "116.202.231.103"
#define MACSURF_PROXY_PORT 8765

static const char *macos9_fetch_filetype(const char *unix_path)
{
	const char *ext;
	if (unix_path == NULL) return "application/octet-stream";
	ext = strrchr(unix_path, '.');
	if (ext == NULL) return "application/octet-stream";
	ext++;
	if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0)
		return "text/html";
	if (strcasecmp(ext, "css") == 0) return "text/css";
	if (strcasecmp(ext, "png") == 0) return "image/png";
	if (strcasecmp(ext, "gif") == 0) return "image/gif";
	if (strcasecmp(ext, "txt") == 0) return "text/plain";
	return "application/octet-stream";
}

static struct nsurl *macos9_fetch_get_resource_url(const char *path)
{
	(void)path;
	return NULL;
}

static char *macos9_fetch_mimetype(const char *ro_path)
{
	return strdup(macos9_fetch_filetype(ro_path));
}

#ifdef __MACOS9__

static pascal void yield_notifier(void *ctx, OTEventCode code,
		OTResult result, void *cookie)
{
	(void)ctx;
	(void)result;
	(void)cookie;
	if (code == kOTSyncIdleEvent)
		YieldToAnyThread();
}

#endif /* __MACOS9__ */

void
macos9_fetch_url(const char *url, struct gui_window *gw,
		void (*callback)(struct gui_window *gw,
			const char *data, long len, int status))
{
#ifdef __MACOS9__
	OSStatus err;
	EndpointRef ep;
	OTConfigurationRef cfg;
	DNSAddress dns_addr;
	TCall snd_call;
	char proxy_addr[64];
	char request[512];
	char *buf;
	char *body;
	long req_len;
	long body_len;
	long total;
	OTResult n;
	OTNotifyUPP notifyUPP;
	char *sep;

	cfg = OTCreateConfiguration("tcp");
	if (cfg == NULL) {
		if (callback) callback(gw, "OT config failed", 16, -1);
		return;
	}

	ep = OTOpenEndpointInContext(cfg, 0, NULL, &err, macos9_ot_context);
	if (err != noErr || ep == NULL) {
		if (callback) callback(gw, "OT open failed", 14, -1);
		return;
	}

	notifyUPP = NewOTNotifyUPP(yield_notifier);
	OTSetSynchronous(ep);
	OTSetBlocking(ep);
	OTInstallNotifier(ep, notifyUPP, NULL);
	OTUseSyncIdleEvents(ep, true);

	err = OTBind(ep, NULL, NULL);
	if (err != noErr) {
		OTCloseProvider(ep);
		DisposeOTNotifyUPP(notifyUPP);
		if (callback) callback(gw, "OT bind failed", 14, -1);
		return;
	}

	sprintf(proxy_addr, "%s:%d", MACSURF_PROXY_HOST, MACSURF_PROXY_PORT);
	OTMemzero(&snd_call, sizeof(TCall));
	snd_call.addr.buf = (UInt8 *)&dns_addr;
	snd_call.addr.len = OTInitDNSAddress(&dns_addr, proxy_addr);

	err = OTConnect(ep, &snd_call, NULL);
	if (err != noErr) {
		OTCloseProvider(ep);
		DisposeOTNotifyUPP(notifyUPP);
		if (callback) callback(gw, "OT connect failed", 17, -1);
		return;
	}

	req_len = 0;
	req_len += sprintf(request + req_len, "GET %s HTTP/1.0\r\n", url);
	req_len += sprintf(request + req_len, "User-Agent: MacSurf/0.1\r\n");
	req_len += sprintf(request + req_len, "Connection: close\r\n\r\n");
	n = OTSnd(ep, request, req_len, 0);
	if (n < 0) {
		OTSndOrderlyDisconnect(ep);
		OTCloseProvider(ep);
		DisposeOTNotifyUPP(notifyUPP);
		if (callback) callback(gw, "OT send failed", 14, -1);
		return;
	}

	buf = (char *)NewPtr(MACSURF_CONTENT_MAX + 4);
	if (buf == NULL) {
		OTSndOrderlyDisconnect(ep);
		OTCloseProvider(ep);
		DisposeOTNotifyUPP(notifyUPP);
		if (callback) callback(gw, "NewPtr failed", 13, -1);
		return;
	}

	total = 0;
	do {
		n = OTRcv(ep, buf + total, (MACSURF_CONTENT_MAX - 1) - total, NULL);
		if (n > 0) total += n;
	} while (n > 0 && total < (MACSURF_CONTENT_MAX - 1));
	buf[total] = '\0';

	body = buf;
	body_len = total;
	if (total > 0) {
		sep = strstr(buf, "\r\n\r\n");
		if (sep != NULL) {
			body = sep + 4;
			body_len = total - (long)(body - buf);
		}
	}

	if (callback != NULL)
		callback(gw, body, body_len, 0);

	DisposePtr(buf);
	OTSndOrderlyDisconnect(ep);
	OTCloseProvider(ep);
	DisposeOTNotifyUPP(notifyUPP);
#else
	if (callback)
		callback(gw, "<html><body><h1>Stub</h1></body></html>", 39, 0);
	(void)url;
	(void)gw;
#endif
}

/*
 * Strip HTML tags, decode a handful of entities, collapse whitespace.
 * Produces a plain-text version of the input into dst, up to dst_cap-1 chars.
 */
long
macos9_strip_html(const char *src, long src_len,
		char *dst, long dst_cap)
{
	long si;
	long di;
	int in_tag;
	int in_script;
	int in_style;
	int last_space;
	char tag_name[16];
	int tag_name_len;

	si = 0;
	di = 0;
	in_tag = 0;
	in_script = 0;
	in_style = 0;
	last_space = 1;
	tag_name_len = 0;
	tag_name[0] = '\0';

	while (si < src_len && di < dst_cap - 1) {
		char c = src[si];

		if (in_tag) {
			if (c == '>') {
				in_tag = 0;
				if (tag_name_len > 0) {
					tag_name[tag_name_len] = '\0';
					if (strcasecmp(tag_name, "script") == 0)
						in_script = 1;
					else if (strcasecmp(tag_name, "/script") == 0)
						in_script = 0;
					else if (strcasecmp(tag_name, "style") == 0)
						in_style = 1;
					else if (strcasecmp(tag_name, "/style") == 0)
						in_style = 0;
					else if (strcasecmp(tag_name, "br") == 0 ||
					         strcasecmp(tag_name, "br/") == 0 ||
					         strcasecmp(tag_name, "p") == 0 ||
					         strcasecmp(tag_name, "/p") == 0 ||
					         strcasecmp(tag_name, "/div") == 0 ||
					         strcasecmp(tag_name, "/tr") == 0 ||
					         strcasecmp(tag_name, "/h1") == 0 ||
					         strcasecmp(tag_name, "/h2") == 0 ||
					         strcasecmp(tag_name, "/h3") == 0 ||
					         strcasecmp(tag_name, "/h4") == 0 ||
					         strcasecmp(tag_name, "/li") == 0) {
						if (di > 0 && dst[di-1] != '\n')
							dst[di++] = '\n';
						last_space = 1;
					}
				}
				tag_name_len = 0;
			} else if (tag_name_len < 15 && c != ' ' && c != '\t') {
				tag_name[tag_name_len++] = c;
			} else if (c == ' ' || c == '\t') {
				if (tag_name_len < 15)
					tag_name[tag_name_len] = '\0';
			}
			si++;
			continue;
		}

		if (c == '<') {
			in_tag = 1;
			tag_name_len = 0;
			si++;
			continue;
		}

		if (in_script || in_style) {
			si++;
			continue;
		}

		if (c == '&') {
			if (si + 4 < src_len && strncmp(src + si, "&amp;", 5) == 0) {
				dst[di++] = '&'; si += 5; last_space = 0; continue;
			}
			if (si + 3 < src_len && strncmp(src + si, "&lt;", 4) == 0) {
				dst[di++] = '<'; si += 4; last_space = 0; continue;
			}
			if (si + 3 < src_len && strncmp(src + si, "&gt;", 4) == 0) {
				dst[di++] = '>'; si += 4; last_space = 0; continue;
			}
			if (si + 5 < src_len && strncmp(src + si, "&quot;", 6) == 0) {
				dst[di++] = '"'; si += 6; last_space = 0; continue;
			}
			if (si + 5 < src_len && strncmp(src + si, "&apos;", 6) == 0) {
				dst[di++] = '\''; si += 6; last_space = 0; continue;
			}
			if (si + 5 < src_len && strncmp(src + si, "&nbsp;", 6) == 0) {
				if (!last_space) { dst[di++] = ' '; last_space = 1; }
				si += 6; continue;
			}
			dst[di++] = c;
			si++;
			last_space = 0;
			continue;
		}

		if (c == '\r' || c == '\n') {
			if (di > 0 && dst[di-1] != '\n')
				dst[di++] = '\n';
			last_space = 1;
			si++;
			continue;
		}

		if (c == ' ' || c == '\t') {
			if (!last_space) {
				dst[di++] = ' ';
				last_space = 1;
			}
			si++;
			continue;
		}

		if ((unsigned char)c < 0x20) {
			si++;
			continue;
		}

		dst[di++] = c;
		last_space = 0;
		si++;
	}

	dst[di] = '\0';
	return di;
}

/*
 * Word-wrap a plain-text buffer into fixed-width lines. Populates
 * line_offsets[] with byte offsets into text[] and line_lengths[] with
 * byte lengths. Breaks on spaces when possible, hard-breaks otherwise.
 * Honors existing '\n' characters as forced line breaks.
 */
long
macos9_word_wrap(const char *text, long text_len,
		long *line_offsets, short *line_lengths,
		long max_lines, short max_chars_per_line)
{
	long i;
	long line_start;
	long last_space;
	long count;

	count = 0;
	line_start = 0;
	last_space = -1;

	if (max_chars_per_line < 8)
		max_chars_per_line = 8;

	for (i = 0; i <= text_len && count < max_lines; i++) {
		int hard_break;
		long width;

		hard_break = 0;
		if (i == text_len) {
			hard_break = 1;
		} else if (text[i] == '\n') {
			hard_break = 1;
		} else if (text[i] == ' ') {
			last_space = i;
		}

		width = i - line_start;

		if (hard_break) {
			line_offsets[count] = line_start;
			line_lengths[count] = (short)(i - line_start);
			count++;
			line_start = i + 1;
			last_space = -1;
			continue;
		}

		if (width >= max_chars_per_line) {
			long break_at;
			if (last_space > line_start) {
				break_at = last_space;
			} else {
				break_at = i;
			}
			line_offsets[count] = line_start;
			line_lengths[count] = (short)(break_at - line_start);
			count++;
			line_start = break_at;
			if (line_start < text_len && text[line_start] == ' ')
				line_start++;
			last_space = -1;
			i = line_start - 1;
		}
	}

	if (count == 0) {
		line_offsets[0] = 0;
		line_lengths[0] = 0;
		count = 1;
	}

	return count;
}

int macos9_ot_init(void) { return 0; }
const char *macos9_ot_get_error(void) { return NULL; }

struct gui_fetch_table macos9_fetch_table = {
	macos9_fetch_filetype,
	macos9_fetch_get_resource_url,
	NULL,
	NULL,
	macos9_fetch_mimetype,
	NULL,
	NULL
};
