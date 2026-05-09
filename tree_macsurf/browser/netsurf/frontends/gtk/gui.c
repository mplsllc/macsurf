/*
 * Copyright 2004-2010 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2010-2016 Vincent Sanders <vince@netsurf-browser.org>
 * Copyright 2004-2009 John-Mark Bell <jmb@netsurf-browser.org>
 * Copyright 2009 Paul Blokus <paul_pl@users.sourceforge.net>
 * Copyright 2006-2009 Daniel Silverstone <dsilvers@netsurf-browser.org>
 * Copyright 2006-2008 Rob Kendrick <rjek@netsurf-browser.org>
 * Copyright 2008 John Tytgat <joty@netsurf-browser.org>
 * Copyright 2008 Adam Blokus <adamblokus@gmail.com>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
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

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <gtk/gtk.h>

#include "utils/filepath.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/utils.h"
#include "utils/file.h"
#include "utils/nsoption.h"
#include "netsurf/keypress.h"
#include "netsurf/url_db.h"
#include "netsurf/cookie_db.h"
#include "netsurf/browser.h"
#include "netsurf/browser_window.h"
#include "netsurf/netsurf.h"
#include "netsurf/bitmap.h"
#include "content/fetch.h"
#include "content/backing_store.h"
#include "desktop/save_complete.h"
#include "desktop/searchweb.h"
#include "desktop/hotlist.h"

#include "gtk/compat.h"
#include "gtk/warn.h"
#include "gtk/completion.h"
#include "gtk/cookies.h"
#include "gtk/download.h"
#include "gtk/fetch.h"
#include "gtk/gui.h"
#include "gtk/local_history.h"
#include "gtk/global_history.h"
#include "gtk/hotlist.h"
#include "gtk/throbber.h"
#include "gtk/toolbar_items.h"
#include "gtk/scaffolding.h"
#include "gtk/window.h"
#include "gtk/corewindow.h"
#include "gtk/schedule.h"
#include "gtk/selection.h"
#include "gtk/search.h"
#include "gtk/bitmap.h"
#include "gtk/misc.h"
#include "gtk/resources.h"
#include "gtk/layout_pango.h"
#include "gtk/accelerator.h"

bool nsgtk_complete = false;

/* exported global defined in gtk/gui.h */
char *nsgtk_config_home;

/** favicon default pixbuf */
GdkPixbuf *favicon_pixbuf;

/** default window icon pixbuf */
GdkPixbuf *win_default_icon_pixbuf;

GtkBuilder *warning_builder;

/** resource search path vector */
char **respaths;


/* exported function documented in gtk/warn.h */
nserror nsgtk_warning(const char *warning, const char *detail)
{
	char buf[300];	/* 300 is the size the RISC OS GUI uses */
	static GtkWindow *nsgtk_warning_window;
	GtkLabel *WarningLabel;

	NSLOG(netsurf, INFO, "%s %s", warning, detail ? detail : "");
	fflush(stdout);

	nsgtk_warning_window = GTK_WINDOW(gtk_builder_get_object(warning_builder, "wndWarning"));
	WarningLabel = GTK_LABEL(gtk_builder_get_object(warning_builder,
							"labelWarning"));

	snprintf(buf, sizeof(buf), "%s %s", messages_get(warning),
		 detail ? detail : "");
	buf[sizeof(buf) - 1] = 0;

	gtk_label_set_text(WarningLabel, buf);

	gtk_widget_show_all(GTK_WIDGET(nsgtk_warning_window));

	return NSERROR_OK;
}


/* exported interface documented in gtk/gui.h */
uint32_t gtk_gui_gdkkey_to_nskey(GdkEventKey *key)
{
	/* this function will need to become much more complex to support
	 * everything that the RISC OS version does.  But this will do for
	 * now.  I hope.
	 */
	switch (key->keyval) {

	case GDK_KEY(Tab):
		return NS_KEY_TAB;

	case GDK_KEY(BackSpace):
		if (key->state & GDK_SHIFT_MASK)
			return NS_KEY_DELETE_LINE_START;
		else if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_DELETE_WORD_LEFT;
		else
			return NS_KEY_DELETE_LEFT;

	case GDK_KEY(Delete):
		if (key->state & GDK_SHIFT_MASK)
			return NS_KEY_DELETE_LINE_END;
		else if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_DELETE_WORD_RIGHT;
		else
			return NS_KEY_DELETE_RIGHT;

	case GDK_KEY(Linefeed):
		return 13;

	case GDK_KEY(Return):
		return 10;

	case GDK_KEY(Left):
	case GDK_KEY(KP_Left):
		if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_WORD_LEFT;
		return NS_KEY_LEFT;

	case GDK_KEY(Right):
	case GDK_KEY(KP_Right):
		if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_WORD_RIGHT;
		return NS_KEY_RIGHT;

	case GDK_KEY(Up):
	case GDK_KEY(KP_Up):
		return NS_KEY_UP;

	case GDK_KEY(Down):
	case GDK_KEY(KP_Down):
		return NS_KEY_DOWN;

	case GDK_KEY(Home):
	case GDK_KEY(KP_Home):
		if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_LINE_START;
		else
			return NS_KEY_TEXT_START;

	case GDK_KEY(End):
	case GDK_KEY(KP_End):
		if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_LINE_END;
		else
			return NS_KEY_TEXT_END;

	case GDK_KEY(Page_Up):
	case GDK_KEY(KP_Page_Up):
		return NS_KEY_PAGE_UP;

	case GDK_KEY(Page_Down):
	case GDK_KEY(KP_Page_Down):
		return NS_KEY_PAGE_DOWN;

	case 'a':
		if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_SELECT_ALL;
		return gdk_keyval_to_unicode(key->keyval);

	case 'u':
		if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_DELETE_LINE;
		return gdk_keyval_to_unicode(key->keyval);

	case 'c':
		if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_COPY_SELECTION;
		return gdk_keyval_to_unicode(key->keyval);

	case 'v':
		if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_PASTE;
		return gdk_keyval_to_unicode(key->keyval);

	case 'x':
		if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_CUT_SELECTION;
		return gdk_keyval_to_unicode(key->keyval);

	case 'Z':
	case 'y':
		if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_REDO;
		return gdk_keyval_to_unicode(key->keyval);

	case 'z':
		if (key->state & GDK_CONTROL_MASK)
			return NS_KEY_UNDO;
		return gdk_keyval_to_unicode(key->keyval);

	case GDK_KEY(Escape):
		return NS_KEY_ESCAPE;

		/* Modifiers - do nothing for now */
	case GDK_KEY(Shift_L):
	case GDK_KEY(Shift_R):
	case GDK_KEY(Control_L):
	case GDK_KEY(Control_R):
	case GDK_KEY(Caps_Lock):
	case GDK_KEY(Shift_Lock):
	case GDK_KEY(Meta_L):
	case GDK_KEY(Meta_R):
	case GDK_KEY(Alt_L):
	case GDK_KEY(Alt_R):
	case GDK_KEY(Super_L):
	case GDK_KEY(Super_R):
	case GDK_KEY(Hyper_L):
	case GDK_KEY(Hyper_R):
		return 0;

	}
	return gdk_keyval_to_unicode(key->keyval);
}


/**
 * Create an array of valid paths to search for resources.
 *
 * The idea is that all the complex path computation to find resources
 * is performed here, once, rather than every time a resource is
 * searched for.
 */
static char **
nsgtk_init_resource_path(const char *config_home)
{
	char *resource_path;
	int resource_path_len;
	const gchar * const *langv;
	char **pathv; /* resource path string vector */
	char **respath; /* resource paths vector */

	if (config_home != NULL) {
		resource_path_len = snprintf(NULL, 0,
					     "%s:${NETSURFRES}:%s",
					     config_home,
					     GTK_RESPATH);
		resource_path = malloc(resource_path_len + 1);
		if (resource_path == NULL) {
			return NULL;
		}
		snprintf(resource_path, resource_path_len + 1,
			 "%s:${NETSURFRES}:%s",
			 config_home,
			 GTK_RESPATH);
	} else {
		resource_path_len = snprintf(NULL, 0,
					     "${NETSURFRES}:%s",
					     GTK_RESPATH);
		resource_path = malloc(resource_path_len + 1);
		if (resource_path == NULL) {
			return NULL;
		}
		snprintf(resource_path,
			 resource_path_len + 1,
			 "${NETSURFRES}:%s",
			 GTK_RESPATH);
	}

	pathv = filepath_path_to_strvec(resource_path);

	langv = g_get_language_names();

	respath = filepath_generate(pathv, langv);

	filepath_free_strvec(pathv);

	free(resource_path);

	return respath;
}


/**
 * create directory name and check it is acessible and a directory.
 */
static nserror
check_dirname(const char *path, const char *leaf, char **dirname_out)
{
	nserror ret;
	char *dirname = NULL;
	struct stat dirname_stat;

	ret = netsurf_mkpath(&dirname, NULL, 2, path, leaf);
	if (ret != NSERROR_OK) {
		return ret;
	}

	/* ensure access is possible and the entry is actualy
	 * a directory.
	 */
	if (stat(dirname, &dirname_stat) == 0) {
		if (S_ISDIR(dirname_stat.st_mode)) {
			if (access(dirname, R_OK | W_OK) == 0) {
				*dirname_out = dirname;
				return NSERROR_OK;
			} else {
				ret = NSERROR_PERMISSION;
			}
		} else {
			ret = NSERROR_NOT_DIRECTORY;
		}
	} else {
		ret = NSERROR_NOT_FOUND;
	}

	free(dirname);

	return ret;
}


/**
 * Get the path to the config directory.
 *
 * @param config_home_out Path to configuration directory.
 * @return NSERROR_OK on sucess and \a config_home_out updated else error code.
 */
static nserror get_config_home(char **config_home_out)
{
	nserror ret;
	char *home_dir;
	char *xdg_config_dir;
	char *config_home;

	home_dir = getenv("HOME");

	/* The old $HOME/.netsurf/ directory should be used if it
	 * exists and is accessible.
	 */
	if (home_dir != NULL) {
		ret = check_dirname(home_dir, ".netsurf", &config_home);
		if (ret == NSERROR_OK) {
			NSLOG(netsurf, INFO, "\"%s\"", config_home);
			*config_home_out = config_home;
			return ret;
		}
	}

	/* $XDG_CONFIG_HOME defines the base directory
	 * relative to which user specific configuration files
	 * should be stored.
	 */
	xdg_config_dir = getenv("XDG_CONFIG_HOME");

	if ((xdg_config_dir == NULL) || (*xdg_config_dir == 0)) {
		/* If $XDG_CONFIG_HOME is either not set or empty, a
		 * default equal to $HOME/.config should be used.
		 */

		/** @todo the meaning of empty is never defined so I
		 * am assuming it is a zero length string but is it
		 * supposed to mean "whitespace" and if so what counts
		 * as whitespace? (are tabs etc. counted or should
		 * isspace() be used)
		 */

		/* the HOME envvar is required */
		if (home_dir == NULL) {
			return NSERROR_NOT_DIRECTORY;
		}

		ret = check_dirname(home_dir, ".config/netsurf", &config_home);
		if (ret != NSERROR_OK) {
			return ret;
		}
	} else {
		ret = check_dirname(xdg_config_dir, "netsurf", &config_home);
		if (ret != NSERROR_OK) {
			return ret;
		}
	}

	NSLOG(netsurf, INFO, "\"%s\"", config_home);

	*config_home_out = config_home;
	return NSERROR_OK;
}


static nserror create_config_home(char **config_home_out)
{
	char *config_home = NULL;
	char *home_dir;
	char *xdg_config_dir;
	nserror ret;

	NSLOG(netsurf, INFO, "Attempting to create configuration directory");

	/* $XDG_CONFIG_HOME defines the base directory
	 * relative to which user specific configuration files
	 * should be stored.
	 */
	xdg_config_dir = getenv("XDG_CONFIG_HOME");

	if ((xdg_config_dir == NULL) || (*xdg_config_dir == 0)) {
		home_dir = getenv("HOME");

		if ((home_dir == NULL) || (*home_dir == 0)) {
			return NSERROR_NOT_DIRECTORY;
		}

		ret = netsurf_mkpath(&config_home, NULL, 4, home_dir, ".config","netsurf", "/");
		if (ret != NSERROR_OK) {
			return ret;
		}
	} else {
		ret = netsurf_mkpath(&config_home, NULL, 3, xdg_config_dir, "netsurf", "/");
		if (ret != NSERROR_OK) {
			return ret;
		}
	}

	/* ensure all elements of path exist (the trailing / is required) */
	ret = netsurf_mkdir_all(config_home);
	if (ret != NSERROR_OK) {
		free(config_home);
		return ret;
	}

	/* strip the trailing separator */
	config_home[strlen(config_home) - 1] = 0;

	NSLOG(netsurf, INFO, "\"%s\"", config_home);

	*config_home_out = config_home;

	return NSERROR_OK;
}


/**
 * Ensures output logging stream is correctly configured
 */
static bool nslog_stream_configure(FILE *fptr)
{
	/* set log stream to be non-buffering */
	setbuf(fptr, NULL);

	return true;
}


/**
 * Set option defaults for gtk frontend.
 *
 * \param defaults The option table to update.
 * \return error status.
 */
static nserror set_defaults(struct nsoption_s *defaults)
{
	char *fname;
	GtkSettings *settings;
	GtkIconSize tooliconsize;
	GtkToolbarStyle toolbarstyle;

	/* cookie file default */
	fname = NULL;
	netsurf_mkpath(&fname, NULL, 2, nsgtk_config_home, "Cookies");
	if (fname != NULL) {
		nsoption_setnull_charp(cookie_file, fname);
	}

	/* cookie jar default */
	fname = NULL;
	netsurf_mkpath(&fname, NULL, 2, nsgtk_config_home, "Cookies");
	if (fname != NULL) {
		nsoption_setnull_charp(cookie_jar, fname);
	}

	/* url database default */
	fname = NULL;
	netsurf_mkpath(&fname, NULL, 2, nsgtk_config_home, "URLs");
	if (fname != NULL) {
		nsoption_setnull_charp(url_file, fname);
	}

	/* bookmark database default */
	fname = NULL;
	netsurf_mkpath(&fname, NULL, 2, nsgtk_config_home, "Hotlist");
	if (fname != NULL) {
		nsoption_setnull_charp(hotlist_path, fname);
	}

	/* download directory default */
	fname = getenv("HOME");
	if (fname != NULL) {
		nsoption_setnull_charp(downloads_directory, strdup(fname));
	}

	if ((nsoption_charp(cookie_file) == NULL) ||
	    (nsoption_charp(cookie_jar) == NULL) ||
	    (nsoption_charp(url_file) == NULL) ||
	    (nsoption_charp(hotlist_path) == NULL) ||
	    (nsoption_charp(downloads_directory) == NULL)) {
		NSLOG(netsurf, INFO,
		      "Failed initialising default resource paths");
		return NSERROR_BAD_PARAMETER;
	}

	/* set default font names */
	nsoption_set_charp(font_sans, strdup("Sans"));
	nsoption_set_charp(font_serif, strdup("Serif"));
	nsoption_set_charp(font_mono, strdup("Monospace"));
	nsoption_set_charp(font_cursive, strdup("Serif"));
	nsoption_set_charp(font_fantasy, strdup("Serif"));

	/* Default toolbar button type to system defaults */

	settings = gtk_settings_get_default();
	g_object_get(settings,
		     "gtk-toolbar-icon-size", &tooliconsize,
		     "gtk-toolbar-style", &toolbarstyle, NULL);

	switch (toolbarstyle) {
	case GTK_TOOLBAR_ICONS:
		if (tooliconsize == GTK_ICON_SIZE_SMALL_TOOLBAR) {
			nsoption_set_int(button_type, 1);
		} else {
			nsoption_set_int(button_type, 2);
		}
		break;

	case GTK_TOOLBAR_TEXT:
		nsoption_set_int(button_type, 4);
		break;

	case GTK_TOOLBAR_BOTH:
	case GTK_TOOLBAR_BOTH_HORIZ:
		/* no labels in default configuration */
	default:
		/* No system default, so use large icons */
		nsoption_set_int(button_type, 2);
		break;
	}

	/* set default items in toolbar */
	nsoption_set_charp(toolbar_items,
			   strdup("back/history/forward/reloadstop/url_bar/websearch/openmenu"));

	/* set default for menu and tool bar visibility */
	nsoption_set_charp(bar_show, strdup("tool"));

	return NSERROR_OK;
}


/**
 * Initialise user options
 *
 * Initialise the browser configuration options. These are set by:
 *  - set generic defaults suitable for the gtk frontend
 *  - user choices loaded from Choices file
 *  - command line parameters
 */
static nserror nsgtk_option_init(int *pargc, char** argv)
{
	nserror ret;
	char *choices = NULL;

	/* user options setup */
	ret = nsoption_init(set_defaults, &nsoptions, &nsoptions_default);
	if (ret != NSERROR_OK) {
		return ret;
	}

	/* Attempt to load the user choices */
	ret = netsurf_mkpath(&choices, NULL, 2, nsgtk_config_home, "Choices");
	if (ret == NSERROR_OK) {
		nsoption_read(choices, nsoptions);
		free(choices);
	}

	/* overide loaded options with those from commandline */
	nsoption_commandline(pargc, argv, nsoptions);

	/* ensure all options fall within sensible bounds */

	/* Attempt to handle nonsense status bar widths.  These may exist
	 * in people's Choices as the GTK front end used to abuse the
	 * status bar width option by using it for an absolute value in px.
	 * The GTK front end now correctly uses it as a proportion of window
	 * width.  Here we assume that a value of less than 15% is wrong
	 * and set to the default two thirds. */
	if (nsoption_int(toolbar_status_size) < 1500) {
		nsoption_set_int(toolbar_status_size, 6667);
	}

	return NSERROR_OK;
}


/**
 * initialise message translation
 */
static nserror nsgtk_messages_init(char **respaths)
{
	const char *messages;
	nserror ret;
	const uint8_t *data;
	size_t data_size;

	ret = nsgtk_data_from_resname("Messages", &data, &data_size);
	if (ret == NSERROR_OK) {
		ret = messages_add_from_inline(data, data_size);
	} else {
		/* Obtain path to messages */
		ret = nsgtk_path_from_resname("Messages", &messages);
		if (ret == NSERROR_OK) {
			ret = messages_add_from_file(messages);
		}
	}
	return ret;
}


/**
 * Get the path to the cache directory.
 *
 * @param cache_home_out Path to cache directory.
 * @return NSERROR_OK on sucess and \a cache_home_out updated else error code.
 */
static nserror get_cache_home(char **cache_home_out)
{
	nserror ret;
	char *xdg_cache_dir;
	char *cache_home;
	char *home_dir;

	/* $XDG_CACHE_HOME defines the base directory relative to
	 * which user specific non-essential data files should be
	 * stored.
	 */
	xdg_cache_dir = getenv("XDG_CACHE_HOME");

	if ((xdg_cache_dir == NULL) || (*xdg_cache_dir == 0)) {
		/* If $XDG_CACHE_HOME is either not set or empty, a
		 * default equal to $HOME/.cache should be used.
		 */

		home_dir = getenv("HOME");

		/* the HOME envvar is required */
		if (home_dir == NULL) {
			return NSERROR_NOT_DIRECTORY;
		}

		ret = check_dirname(home_dir, ".cache/netsurf", &cache_home);
		if (ret != NSERROR_OK) {
			return ret;
		}
	} else {
		ret = check_dirname(xdg_cache_dir, "netsurf", &cache_home);
		if (ret != NSERROR_OK) {
			return ret;
		}
	}

	NSLOG(netsurf, INFO, "\"%s\"", cache_home);

	*cache_home_out = cache_home;
	return NSERROR_OK;
}


/**
 * create a cache directory
 */
static nserror create_cache_home(char **cache_home_out)
{
	char *cache_home = NULL;
	char *home_dir;
	char *xdg_cache_dir;
	nserror ret;

	NSLOG(netsurf, INFO, "Attempting to create cache directory");

	/* $XDG_CACHE_HOME defines the base directory
	 * relative to which user specific cache files
	 * should be stored.
	 */
	xdg_cache_dir = getenv("XDG_CACHE_HOME");

	if ((xdg_cache_dir == NULL) || (*xdg_cache_dir == 0)) {
		home_dir = getenv("HOME");

		if ((home_dir == NULL) || (*home_dir == 0)) {
			return NSERROR_NOT_DIRECTORY;
		}

		ret = netsurf_mkpath(&cache_home, NULL, 4, home_dir, ".cache", "netsurf", "/");
		if (ret != NSERROR_OK) {
			return ret;
		}
	} else {
		ret = netsurf_mkpath(&cache_home, NULL, 3, xdg_cache_dir, "netsurf", "/");
		if (ret != NSERROR_OK) {
			return ret;
		}
	}

	/* ensure all elements of path exist (the trailing / is required) */
	ret = netsurf_mkdir_all(cache_home);
	if (ret != NSERROR_OK) {
		free(cache_home);
		return ret;
	}

	/* strip the trailing separator */
	cache_home[strlen(cache_home) - 1] = 0;

	NSLOG(netsurf, INFO, "\"%s\"", cache_home);

	*cache_home_out = cache_home;

	return NSERROR_OK;
}


/**
 * GTK specific initialisation
 */
static nserror nsgtk_init(int *pargc, char ***pargv, char **cache_home)
{
	nserror ret;

	/* Locate the correct user configuration directory path */
	ret = get_config_home(&nsgtk_config_home);
	if (ret == NSERROR_NOT_FOUND) {
		/* no config directory exists yet so try to create one */
		ret = create_config_home(&nsgtk_config_home);
	}
	if (ret != NSERROR_OK) {
		NSLOG(netsurf, INFO,
		      "Unable to locate a configuration directory.");
		nsgtk_config_home = NULL;
	}

	/* Initialise gtk */
	gtk_init(pargc, pargv);

	/* initialise logging. Not fatal if it fails but not much we
	 * can do about it either.
	 */
	nslog_init(nslog_stream_configure, pargc, *pargv);

	/* build the common resource path list */
	respaths = nsgtk_init_resource_path(nsgtk_config_home);
	if (respaths == NULL) {
		fprintf(stderr, "Unable to locate resources\n");
		return 1;
	}

	/* initialise the gtk resource handling */
	ret = nsgtk_init_resources(respaths);
	if (ret != NSERROR_OK) {
		fprintf(stderr, "GTK resources failed to initialise (%s)\n",
			messages_get_errorcode(ret));
		return ret;
	}

	/* Initialise user options */
	ret = nsgtk_option_init(pargc, *pargv);
	if (ret != NSERROR_OK) {
		fprintf(stderr, "Options failed to initialise (%s)\n",
			messages_get_errorcode(ret));
		return ret;
	}

	/* Initialise translated messages */
	ret = nsgtk_messages_init(respaths);
	if (ret != NSERROR_OK) {
		fprintf(stderr, "Unable to load translated messages (%s)\n",
			messages_get_errorcode(ret));
		NSLOG(netsurf, INFO, "Unable to load translated messages");
		/** \todo decide if message load faliure should be fatal */
	}

	/* Locate the correct user cache directory path */
	ret = get_cache_home(cache_home);
	if (ret == NSERROR_NOT_FOUND) {
		/* no cache directory exists yet so try to create one */
		ret = create_cache_home(cache_home);
	}
	if (ret != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Unable to locate a cache directory.");
	}


	return NSERROR_OK;
}


#if GTK_CHECK_VERSION(3,14,0)

/**
 * adds named icons into gtk theme
 */
static nserror nsgtk_add_named_icons_to_theme(void)
{
	gtk_icon_theme_add_resource_path(gtk_icon_theme_get_default(),
					 "/org/netsurf/icons");
	return NSERROR_OK;
}

#else

static nserror
add_builtin_icon(const char *prefix, const char *name, int x, int y)
{
	GdkPixbuf *pixbuf;
	nserror res;
	char *resname;
	int resnamelen;

	/* resource name string length allowing for / .png and termination */
	resnamelen = strlen(prefix) + strlen(name) + 5 + 1 + 4 + 1;
	resname = malloc(resnamelen);
	if (resname == NULL) {
		return NSERROR_NOMEM;
	}
	snprintf(resname, resnamelen, "icons%s/%s.png", prefix, name);

	res = nsgdk_pixbuf_new_from_resname(resname, &pixbuf);
	NSLOG(netsurf, DEEPDEBUG, "%d %s", res, resname);
	free(resname);
	if (res != NSERROR_OK) {
		pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, false, 8, x, y);
	}
	gtk_icon_theme_add_builtin_icon(name, y, pixbuf);

	return NSERROR_OK;
}


/**
 * adds named icons into gtk theme
 */
static nserror nsgtk_add_named_icons_to_theme(void)
{
	/* these must also be in gtk/resources.c pixbuf_resource *and*
	 * gtk/res/netsurf.gresource.xml
	 */
	add_builtin_icon("", "local-history", 8, 32);
	add_builtin_icon("", "show-cookie", 24, 24);
	add_builtin_icon("/24x24/actions", "page-info-insecure", 24, 24);
	add_builtin_icon("/24x24/actions", "page-info-internal", 24, 24);
	add_builtin_icon("/24x24/actions", "page-info-local", 24, 24);
	add_builtin_icon("/24x24/actions", "page-info-secure", 24, 24);
	add_builtin_icon("/24x24/actions", "page-info-warning", 24, 24);
	add_builtin_icon("/48x48/actions", "page-info-insecure", 48, 48);
	add_builtin_icon("/48x48/actions", "page-info-internal", 48, 48);
	add_builtin_icon("/48x48/actions", "page-info-local", 48, 48);
	add_builtin_icon("/48x48/actions", "page-info-secure", 48, 48);
	add_builtin_icon("/48x48/actions", "page-info-warning", 48, 48);

	return NSERROR_OK;
}

#endif


/**
 * setup GTK specific parts of the browser.
 *
 * \param argc The number of arguments on the command line
 * \param argv A string vector of command line arguments.
 * \respath A string vector of the path elements of resources
 */
static nserror nsgtk_setup(int argc, char** argv, char **respath)
{
	char buf[PATH_MAX];
	char *resource_filename;
	char *addr = NULL;
	nsurl *url;
	nserror res;

	/* Initialise gtk accelerator table */
	res = nsgtk_accelerator_init(respaths);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO,
		      "Unable to load gtk accelerator configuration");
		/* not fatal if this does not load */
	}

	/* initialise warning dialog */
	res = nsgtk_builder_new_from_resname("warning", &warning_builder);
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Unable to initialise warning dialog");
		return res;
	}

	gtk_builder_connect_signals(warning_builder, NULL);

	/* set default icon if its available */
	res = nsgdk_pixbuf_new_from_resname("netsurf.xpm",
					    &win_default_icon_pixbuf);
	if (res == NSERROR_OK) {
		NSLOG(netsurf, INFO, "Seting default window icon");
		gtk_window_set_default_icon(win_default_icon_pixbuf);
	}

	/* Search engine sources */
	resource_filename = filepath_find(respath, "SearchEngines");
	search_web_init(resource_filename);
	if (resource_filename != NULL) {
		NSLOG(netsurf, INFO, "Using '%s' as Search Engines file",
		      resource_filename);
		free(resource_filename);
	}
	search_web_select_provider(nsoption_charp(search_web_provider));

	/* Default favicon */
	res = nsgdk_pixbuf_new_from_resname("favicon.png", &favicon_pixbuf);
	if (res != NSERROR_OK) {
		favicon_pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB,
						false, 8, 16, 16);
	}

	/* add named icons to gtk theme */
	res = nsgtk_add_named_icons_to_theme();
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Unable to add named icons to GTK theme.");
		return res;
	}

	/* initialise throbber */
	res = nsgtk_throbber_init();
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Unable to initialise throbber.");
		return res;
	}

	/* Initialise completions - cannot fail */
	nsgtk_completion_init();

	/* The tree view system needs to know the screen's DPI, so we
	 * find that out here, rather than when we create a first browser
	 * window.
	 */
	browser_set_dpi(gdk_screen_get_resolution(gdk_screen_get_default()));
	NSLOG(netsurf, INFO, "Set CSS DPI to %d", browser_get_dpi());

	bitmap_set_format(&(bitmap_fmt_t) {
		.layout = BITMAP_LAYOUT_ARGB8888,
		.pma = true,
	});

	filepath_sfinddef(respath, buf, "mime.types", "/etc/");
	gtk_fetch_filetype_init(buf);

	save_complete_init();

	urldb_load(nsoption_charp(url_file));
	urldb_load_cookies(nsoption_charp(cookie_file));
	hotlist_init(nsoption_charp(hotlist_path),
		     nsoption_charp(hotlist_path));

	/* Initialise top level UI elements */
	res = nsgtk_download_init();
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Unable to initialise download window.");
		return res;
	}

	/* If there is a url specified on the command line use it */
	if (argc > 1) {
		struct stat fs;
		if (stat(argv[1], &fs) == 0) {
			size_t addrlen;
			char *rp = realpath(argv[1], NULL);
			assert(rp != NULL);

			/* calculate file url length including terminator */
			addrlen = SLEN("file://") + strlen(rp) + 1;
			addr = malloc(addrlen);
			assert(addr != NULL);
			snprintf(addr, addrlen, "file://%s", rp);
			free(rp);
		} else {
			addr = strdup(argv[1]);
		}
	}
	if (addr != NULL) {
		/* managed to set up based on local launch */
	} else if (nsoption_charp(homepage_url) != NULL) {
		addr = strdup(nsoption_charp(homepage_url));
	} else {
		addr = strdup(NETSURF_HOMEPAGE);
	}

	/* create an initial browser window */
	res = nsurl_create(addr, &url);
	if (res == NSERROR_OK) {
		res = browser_window_create(BW_CREATE_HISTORY,
					    url,
					    NULL,
					    NULL,
					    NULL);
		nsurl_unref(url);
	}

	free(addr);

	return res;
}


/**
 * Run the gtk event loop.
 *
 * The same as the standard gtk_main loop except this ensures active
 * FD are added to the gtk poll event set.
 */
static void nsgtk_main(void)
{
	fd_set read_fd_set, write_fd_set, exc_fd_set;
	int max_fd;
	GPollFD *fd_list[1000];
	unsigned int fd_count;

	while (!nsgtk_complete) {
		max_fd = -1;
		fd_count = 0;
		FD_ZERO(&read_fd_set);
		FD_ZERO(&write_fd_set);
		FD_ZERO(&exc_fd_set);

		while (gtk_events_pending())
			gtk_main_iteration_do(TRUE);

		schedule_run();

		fetch_fdset(&read_fd_set, &write_fd_set, &exc_fd_set, &max_fd);
		for (int i = 0; i <= max_fd; i++) {
			if (FD_ISSET(i, &read_fd_set)) {
				GPollFD *fd = malloc(sizeof *fd);
				fd->fd = i;
				fd->events = G_IO_IN | G_IO_HUP | G_IO_ERR;
				g_main_context_add_poll(0, fd, 0);
				fd_list[fd_count++] = fd;
			}
			if (FD_ISSET(i, &write_fd_set)) {
				GPollFD *fd = malloc(sizeof *fd);
				fd->fd = i;
				fd->events = G_IO_OUT | G_IO_ERR;
				g_main_context_add_poll(0, fd, 0);
				fd_list[fd_count++] = fd;
			}
			if (FD_ISSET(i, &exc_fd_set)) {
				GPollFD *fd = malloc(sizeof *fd);
				fd->fd = i;
				fd->events = G_IO_ERR;
				g_main_context_add_poll(0, fd, 0);
				fd_list[fd_count++] = fd;
			}
		}

		gtk_main_iteration();

		for (unsigned int i = 0; i != fd_count; i++) {
			g_main_context_remove_poll(0, fd_list[i]);
			free(fd_list[i]);
		}
	}
}


/**
 * finalise the browser
 */
static void nsgtk_finalise(void)
{
	nserror res;

	NSLOG(netsurf, INFO, "Quitting GUI");

	/* Ensure all scaffoldings are destroyed before we go into exit */
	nsgtk_download_destroy();
	urldb_save_cookies(nsoption_charp(cookie_jar));
	urldb_save(nsoption_charp(url_file));

	res = nsgtk_cookies_destroy();
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Error finalising cookie viewer: %s",
		      messages_get_errorcode(res));
	}

	res = nsgtk_local_history_destroy();
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO,
		      "Error finalising local history viewer: %s",
		      messages_get_errorcode(res));
	}

	res = nsgtk_global_history_destroy();
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO,
		      "Error finalising global history viewer: %s",
		      messages_get_errorcode(res));
	}

	res = nsgtk_hotlist_destroy();
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Error finalising hotlist viewer: %s",
		      messages_get_errorcode(res));
	}

	res = hotlist_fini();
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Error finalising hotlist: %s",
		      messages_get_errorcode(res));
	}

	res = save_complete_finalise();
	if (res != NSERROR_OK) {
		NSLOG(netsurf, INFO, "Error finalising save complete: %s",
		      messages_get_errorcode(res));
	}

	free(nsgtk_config_home);

	gtk_fetch_filetype_fin();

	/* common finalisation */
	netsurf_exit();

	/* finalise options */
	nsoption_finalise(nsoptions, nsoptions_default);

	/* finalise logging */
	nslog_finalise();

}


/**
 * Main entry point from OS.
 */
int main(int argc, char** argv)
{
	nserror res;
	char *cache_home = NULL;
	struct netsurf_table nsgtk_table = {
		.misc = nsgtk_misc_table,
		.window = nsgtk_window_table,
		.corewindow = nsgtk_core_window_table,
		.clipboard = nsgtk_clipboard_table,
		.download = nsgtk_download_table,
		.fetch = nsgtk_fetch_table,
		.llcache = filesystem_llcache_table,
		.search = nsgtk_search_table,
		.search_web = nsgtk_search_web_table,
		.bitmap = nsgtk_bitmap_table,
		.layout = nsgtk_layout_table,
	};

	res = netsurf_register(&nsgtk_table);
	if (res != NSERROR_OK) {
		fprintf(stderr,
			"NetSurf operation table failed registration (%s)\n",
			messages_get_errorcode(res));
		return 1;
	}

	/* gtk specific initialisation */
	res = nsgtk_init(&argc, &argv, &cache_home);
	if (res != NSERROR_OK) {
		fprintf(stderr, "NetSurf gtk failed to initialise (%s)\n",
			messages_get_errorcode(res));
		return 2;
	}

	/* core initialisation */
	res = netsurf_init(cache_home);
	free(cache_home);
	if (res != NSERROR_OK) {
		fprintf(stderr, "NetSurf core failed to initialise (%s)\n",
			messages_get_errorcode(res));
		return 3;
	}

	/* gtk specific initalisation and main run loop */
	res = nsgtk_setup(argc, argv, respaths);
	if (res != NSERROR_OK) {
		nsgtk_finalise();
		fprintf(stderr, "NetSurf gtk setup failed (%s)\n",
			messages_get_errorcode(res));
		return 4;
	}

	nsgtk_main();

	nsgtk_finalise();

	return 0;
}
