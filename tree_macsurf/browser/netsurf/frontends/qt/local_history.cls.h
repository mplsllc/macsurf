/**
 * \file
 * Implementation of local history widget for qt.
 */

#include "qt/corewindow.cls.h"

class NS_Local_history : public NS_Corewindow
{
	Q_OBJECT

public:
	NS_Local_history(QWidget *parent, struct browser_window *bw);
	~NS_Local_history();

	nserror setbw(struct browser_window *bw);

private:
	void setMaximumSize(struct browser_window *bw);
	void draw(struct rect *clip, struct redraw_context *ctx);
	bool key_press(uint32_t nskey);
	void mouse_action(browser_mouse_state mouse_state, int x, int y);

	struct local_history_session *m_session;
};
