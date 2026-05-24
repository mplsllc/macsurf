/*
 * MacSurf - macos9_fetcher_init.c
 *
 * fetcher_init implementation. Replaces NetSurf core's fetcher_init()
 * (#ifdef'd out under __MACOS9__ in content/fetch.c) so we control
 * exactly which scheme fetchers get registered.
 *
 * fixes67: now also registers the resource/about/file/data/javascript
 * stub fetchers from macos9_fetcher_stubs.c. The previous shim only
 * registered the HTTP fetcher, so html_css_new_stylesheets failed with
 * NSERROR_NO_FETCH_HANDLER when it tried to fetch resource:default.css
 * (the default user-agent stylesheet) and the entire content pipeline
 * stalled — content_factory_create_content returned NULL,
 * current_content stayed NULL, redraw painted only the white blank.
 */

#include "utils/errors.h"
#include "macsurf_debug.h"

extern nserror macos9_http_fetcher_register(void);
extern nserror fetch_resource_register(void);
extern nserror fetch_about_register(void);
extern nserror fetch_file_register(void);
extern nserror fetch_data_register(void);
extern nserror fetch_javascript_register(void);

nserror fetcher_init(void) {
	nserror err;

	err = macos9_http_fetcher_register();
	MS_LOG("fetcher_init: http registered");
	if (err != NSERROR_OK) return err;

	err = fetch_resource_register();
	MS_LOG("fetcher_init: resource registered");
	if (err != NSERROR_OK) return err;

	err = fetch_about_register();
	if (err != NSERROR_OK) return err;

	err = fetch_file_register();
	if (err != NSERROR_OK) return err;

	err = fetch_data_register();
	if (err != NSERROR_OK) return err;

	err = fetch_javascript_register();
	MS_LOG("fetcher_init: all stub fetchers registered");
	return err;
}
