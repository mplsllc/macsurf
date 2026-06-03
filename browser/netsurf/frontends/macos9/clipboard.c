/*
 * MacSurf — Mac OS 9 frontend for NetSurf
 * clipboard.c — All gui_clipboard_table callbacks
 *
 * This file is part of MacSurf, built on the NetSurf engine.
 * Licensed under GPL v2.
 *
 * Carbon Scrap Manager path (CarbonLib 1.0+). The classic
 * ZeroScrap/PutScrap/GetScrap A-traps are CALL_NOT_IN_CARBON and will
 * not link in a Carbon binary, so we use GetCurrentScrap /
 * ClearCurrentScrap / PutScrapFlavor / GetScrapFlavorSize /
 * GetScrapFlavorData exclusively (all CarbonLib 1.0 entry points).
 *
 * The desk-scrap 'TEXT' flavor is MacRoman bytes; NetSurf core deals in
 * UTF-8, so get() converts MacRoman -> UTF-8 and set() converts
 * UTF-8 -> MacRoman.
 */

#include <stdlib.h>
#include <string.h>

#include "utils/ns_errors.h"
#include "utils/log.h"
#include "netsurf/clipboard.h"

#include "macos9.h"
#ifdef __MACOS9__
#include <Scrap.h>	/* GetCurrentScrap / PutScrapFlavor / ... (not suppressed) */
#endif

static void
macos9_clipboard_get(char **buffer, size_t *length)
{
#ifdef __MACOS9__
	ScrapRef scrap;
	Size     flavor_size;
	Size     got;
	OSStatus err;
	char    *mac_buf;

	*buffer = NULL;
	*length = 0;

	err = GetCurrentScrap(&scrap);
	if (err != noErr)
		return;

	flavor_size = 0;
	err = GetScrapFlavorSize(scrap, kScrapFlavorTypeText, &flavor_size);
	if (err != noErr)		/* no 'TEXT' present == normal empty path */
		return;
	if (flavor_size <= 0)		/* empty clipboard */
		return;

	mac_buf = (char *)malloc((size_t)flavor_size);
	if (mac_buf == NULL)
		return;

	got = flavor_size;		/* in: capacity; out: bytes written */
	err = GetScrapFlavorData(scrap, kScrapFlavorTypeText, &got, mac_buf);
	if (err != noErr || got <= 0) {
		free(mac_buf);
		return;
	}

	/* MacRoman -> UTF-8. Worst case is 3 UTF-8 bytes per MacRoman byte,
	 * plus a terminating NUL. NetSurf core (textarea.c paste) frees
	 * *buffer. */
	{
		char  *utf8_buf;
		size_t cap;
		size_t out_len;

		cap = (size_t)got * 3 + 1;
		utf8_buf = (char *)malloc(cap);
		if (utf8_buf == NULL) {
			free(mac_buf);
			return;
		}
		out_len = macos9_macroman_to_utf8(
				(const unsigned char *)mac_buf,
				(size_t)got, utf8_buf, cap);
		free(mac_buf);

		*buffer = utf8_buf;	/* caller frees */
		*length = out_len;
	}
#else
	*buffer = NULL;
	*length = 0;
#endif
}

static void
macos9_clipboard_set(const char *buffer, size_t length,
		     nsclipboard_styles styles[], int n_styles)
{
#ifdef __MACOS9__
	ScrapRef scrap;
	OSStatus err;
	char    *mac_buf;
	size_t   mac_cap;
	size_t   mac_len;

	(void)styles;			/* MacRoman 'TEXT' is unstyled */
	(void)n_styles;

	if (buffer == NULL || length == 0)
		return;

	/* UTF-8 -> MacRoman. Most chars shrink (multi-byte UTF-8 -> 1 MacRoman
	 * byte), but a few EXPAND: macos9_utf8_to_macroman maps the vulgar
	 * fractions (1/2, 1/4, 3/4) to the 3-byte ASCII forms "1/2" etc. So the
	 * MacRoman output can exceed the UTF-8 input length; size the buffer at
	 * 3x to never truncate. The encoder does not NUL-terminate; use the
	 * returned count. */
	mac_cap = length * 3 + 1;
	mac_buf = (char *)malloc(mac_cap);
	if (mac_buf == NULL)
		return;
	mac_len = macos9_utf8_to_macroman(buffer, length, mac_buf, mac_cap);
	if (mac_len == 0) {
		free(mac_buf);
		return;
	}

	err = ClearCurrentScrap();	/* required before replacing clipboard */
	if (err != noErr) {
		free(mac_buf);
		return;
	}

	err = GetCurrentScrap(&scrap);
	if (err != noErr) {
		free(mac_buf);
		return;
	}

	(void)PutScrapFlavor(scrap, kScrapFlavorTypeText, kScrapFlavorMaskNone,
			     (Size)mac_len, mac_buf);

	free(mac_buf);
#else
	(void)buffer; (void)length; (void)styles; (void)n_styles;
#endif
}

/* Field order: get, set (see include/netsurf/clipboard.h) */
static struct gui_clipboard_table clipboard_table = {
	macos9_clipboard_get,
	macos9_clipboard_set
};

struct gui_clipboard_table *macos9_clipboard_table = &clipboard_table;
