/*
 * macos9_desktop_stubs.c
 *
 * No-op stubs for the NetSurf desktop UI modules that MacSurf does NOT
 * build: cookie_manager / hotlist / global_history (the treeview-based
 * manager windows) and page-info (the security "padlock" popup).
 *
 * Core files call these hooks unconditionally:
 *   content/urldb.c        -> cookie_manager_add / cookie_manager_remove
 *   desktop/browser_window.c -> global_history_add / hotlist_update_url
 *   desktop/netsurf.c      -> page_info_init / page_info_fini
 *
 * MacSurf handles cookies, history and bookmarks itself via urldb plus its
 * own native chrome (see macos9_chrome_extras.c), so none of the desktop
 * treeview machinery is compiled.  These stubs let the core link cleanly
 * without dragging in treeview.c (which is not C89/CW8-clean) and its
 * knockout/core_window dependency chain.
 *
 * C89 / CodeWarrior 8.
 */

#include "utils/errors.h"

/* Only pointers are taken, so incomplete forward declarations suffice. */
struct cookie_data;
struct nsurl;

bool cookie_manager_add(const struct cookie_data *data)
{
	(void) data;
	return true;
}

void cookie_manager_remove(const struct cookie_data *data)
{
	(void) data;
}

nserror global_history_add(struct nsurl *url)
{
	(void) url;
	return NSERROR_OK;
}

void hotlist_update_url(struct nsurl *url)
{
	(void) url;
}

nserror page_info_init(void)
{
	return NSERROR_OK;
}

nserror page_info_fini(void)
{
	return NSERROR_OK;
}
