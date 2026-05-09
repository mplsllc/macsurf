/*
 * Copyright 2024 Vincent Sanders <vince@netsurf-browser.org>
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

/**
 * \file
 * Application class for QT frontend.
 */

#ifdef fallthrough
#undef fallthrough
#endif

#include <QApplication>
#include <QTimer>
#include <QWidget>

extern "C" {
#include "utils/errors.h"
#include "utils/nsurl.h"
}

#include "qt/local_history.cls.h"
#include "qt/cookies.cls.h"

class NS_Exception {
public:
	std::string m_str;
	nserror m_err;

	NS_Exception(std::string str, nserror err):m_str(str),m_err(err) {}
};

/**
 * class for netsurf application
 */
class NS_Application : public QApplication
{
	Q_OBJECT
public:
	NS_Application(int &argc, char **argv, struct netsurf_table *nsqt_table);
	~NS_Application();

	bool event(QEvent* event) override;

	void next_schedule(int ms);

	void nsOptionPersist();

	/* corewindow show methods */
	void settings_show();
	void bookmarks_show();
	void local_history_show(struct browser_window *bw, const QPoint &pos);
	void page_info_show(struct browser_window *bw, const QPoint &pos);
	void global_history_show();
	nserror cookies_show(const char *search_term = NULL);

	/**
	 * apply any options changes which depend on external system
	 * configuration
	 *
	 * This is for things like updating options when automatic colour is
	 * selected and QT appearance settings changes
	 */
	void nsOptionUpdate();

	/**
	 * get application instance
	 */
	static NS_Application* instance();

	/**
	 * create a new browsing context in a tab or window
	 *
	 * \param url An initial URL to use or NULL to use default.
	 * \param existing an existing browser window or NULL.
	 * \param intab if the new widget should be a window or tab in existing.
	 */
	static nserror create_browser_widget(nsurl *url, struct browser_window *existing, bool intab);

	/**
	 * create a new browsing context in a tab or window
	 *
	 * \param hlchandle A high level cache handle.
	 * \param existing an existing browser window or NULL.
	 * \param intab if the new widget should be a window or tab in existing.
	 */
	static nserror create_browser_widget(struct hlcache_handle *hlchandle, struct browser_window *existing, bool intab);

	/**
	 * create a new browsing context in a tab or window
	 *
	 * \param existing an existing browser window or NULL.
	 * \param intab if the new widget should be a window or tab in existing.
	 */
	static nserror create_browser_widget(struct browser_window *existing, bool intab);

public slots:
	void schedule_run();

private:
	static bool nslog_stream_configure(FILE *fptr);
	/**
	 * Set option defaults for qt frontend
	 *
	 * @param defaults The option table to update.
	 * @return error status.
	 */
	static nserror set_option_defaults(struct nsoption_s *defaults);

	void nsOptionLoad();

	/**
	 * set system-color nsoptions from QT palette
	 */
	static void nsOptionFromPalette(struct nsoption_s *opts);

	QTimer *m_schedule_timer;
	QWidget *m_settings_window;
	QWidget *m_bookmarks_window;
	NS_Local_history *m_local_history_window;
	QWidget *m_global_history_window;
	NS_Cookies *m_cookies_window;
};
