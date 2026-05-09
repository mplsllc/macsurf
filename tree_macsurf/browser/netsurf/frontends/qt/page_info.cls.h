/**
 * \file
 * Implementation of page info widget for qt.
 */

#include "qt/corewindow.cls.h"

class NS_Page_info : public NS_Corewindow
{
	Q_OBJECT

public:
	NS_Page_info(QWidget *parent, struct browser_window *bw);
	~NS_Page_info();

private:
	void draw(struct rect *clip, struct redraw_context *ctx);
	bool key_press(uint32_t nskey);
	void mouse_action(browser_mouse_state mouse_state, int x, int y);

	struct page_info *m_session;
};
