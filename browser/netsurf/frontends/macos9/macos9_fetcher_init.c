/*
 * MacSurf - macos9_fetcher_init.c
 *
 * Mac OS 9 fetcher initialisation.
 *
 * The NetSurf core expects fetcher_init() to register every scheme the
 * browser needs before the first browser_window is created. MacSurf cannot
 * use curl/file/resource fetchers from the desktop build, so this shim wires
 * the local stubs plus the Open Transport HTTP/HTTPS fetcher.
 */

#include "utils/errors.h"

extern nserror fetch_data_register(void);
extern nserror fetch_file_register(void);
extern nserror fetch_resource_register(void);
extern nserror fetch_about_register(void);
extern nserror fetch_javascript_register(void);
extern nserror macos9_fetcher_register(void);

nserror fetcher_init(void)
{
	nserror ret;

	ret = fetch_data_register();
	if (ret != NSERROR_OK) return ret;

	ret = fetch_file_register();
	if (ret != NSERROR_OK) return ret;

	ret = fetch_resource_register();
	if (ret != NSERROR_OK) return ret;

	ret = fetch_about_register();
	if (ret != NSERROR_OK) return ret;

	ret = fetch_javascript_register();
	if (ret != NSERROR_OK) return ret;

	return macos9_fetcher_register();
}
