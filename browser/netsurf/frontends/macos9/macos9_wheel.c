/*
 * MacSurf - macos9_wheel.c
 * Mouse wheel event support via Carbon Events.
 *
 * Classic OS 9 / System 7 shipped without scroll-wheel mice, so the
 * WaitNextEvent classic event model has no dedicated wheel opcode.
 * USB wheel drivers on OS 9.2.x deliver wheel events through the
 * Carbon Event Manager as kEventMouseWheelMoved. When no application
 * handler claims these events, CarbonLib's default handler chain can
 * walk into an uninitialized slot and bus-error on dereference —
 * that is exactly what was observed on 2026-04-20 when a user with a
 * USB wheel mouse rolled the wheel over a MacSurf window.
 *
 * Installing the handler below both prevents that crash and routes
 * wheel deltas through the existing scroll machinery
 * (macos9_window_scroll_by), so wheel scrolling behaves the same as
 * arrow-key / Page-Up / Page-Down scrolling.
 */

#include <string.h>

#include "utils/errors.h"
#include "macos9/macos9.h"
#include "macsurf_debug.h"

#ifdef __MACOS9__

#include <Events.h>
#include <MacWindows.h>
#include <CarbonEvents.h>

/* CW8 Universal Interfaces can miss these Carbon event codes. They
 * are documented in Apple's Carbon Event Manager Reference and are
 * stable four-character codes — define locally rather than add more
 * headers, per MacSurf's CW8-missing-constants convention. */
#ifndef kEventClassMouse
#define kEventClassMouse            'mous'
#endif
#ifndef kEventMouseWheelMoved
#define kEventMouseWheelMoved       10
#endif
#ifndef kEventParamMouseWheelAxis
#define kEventParamMouseWheelAxis   'mwax'
#endif
#ifndef kEventParamMouseWheelDelta
#define kEventParamMouseWheelDelta  'mwdl'
#endif
#ifndef kEventParamWindowRef
#define kEventParamWindowRef        'wind'
#endif
#ifndef typeMouseWheelAxis
#define typeMouseWheelAxis          'mwax'
#endif
#ifndef typeSInt32
#define typeSInt32                  'long'
#endif
#ifndef typeWindowRef
#define typeWindowRef               'wind'
#endif
#ifndef kEventMouseWheelAxisX
#define kEventMouseWheelAxisX       0
#endif
#ifndef kEventMouseWheelAxisY
#define kEventMouseWheelAxisY       1
#endif

/* Pixels scrolled per wheel delta unit. A wheel notch typically
 * delivers delta=1 (stock Apple driver) or delta=3 (some third-party
 * drivers). 40 matches Apple's classic-era default scroll distance. */
#define MACOS9_WHEEL_STEP_PX        40

/* Typedef for the axis parameter. CW8 may or may not provide it; the
 * underlying storage is SInt16. */
typedef short macos9_wheel_axis;


/* One-shot: the first time the Carbon wheel handler actually gets
 * called, latch the title so the user sees it survived install. If
 * they spin the wheel and the title stays whatever layout probes set,
 * the handler is not firing — wheel events are arriving via a
 * different path (likely classic osEvt). */
static int g_whl_fire_logged = 0;

static pascal OSStatus
macos9_wheel_handler(EventHandlerCallRef next_handler,
                     EventRef event,
                     void *user_data)
{
	macos9_wheel_axis axis;
	SInt32 delta;
	WindowRef window;
	OSStatus err;
	struct gui_window *gw;
	int dx;
	int dy;

	(void)next_handler;
	(void)user_data;

	if (!g_whl_fire_logged) {
		g_whl_fire_logged = 1;
		MS_LOG_STICKY("whl fire");
	}

	axis = kEventMouseWheelAxisY;
	delta = 0;
	window = NULL;

	err = GetEventParameter(event,
			kEventParamMouseWheelAxis,
			typeMouseWheelAxis,
			NULL, sizeof(axis), NULL, &axis);
	if (err != noErr)
		axis = kEventMouseWheelAxisY;

	err = GetEventParameter(event,
			kEventParamMouseWheelDelta,
			typeSInt32,
			NULL, sizeof(delta), NULL, &delta);
	if (err != noErr)
		delta = 0;

	err = GetEventParameter(event,
			kEventParamWindowRef,
			typeWindowRef,
			NULL, sizeof(window), NULL, &window);
	if (err != noErr || window == NULL) {
		window = FrontWindow();
	}

	if (window == NULL || delta == 0)
		return noErr;

	gw = macos9_find_window(window);
	if (gw == NULL)
		return noErr;

	/* Mac wheel convention: positive delta => user rolled wheel
	 * upward => content should move down relative to the viewport
	 * (i.e. scroll position decreases). scroll_by takes a positive
	 * dy to mean "move viewport down into the content", so invert. */
	dx = 0;
	dy = 0;
	if (axis == kEventMouseWheelAxisX) {
		dx = -(int)delta * MACOS9_WHEEL_STEP_PX;
	} else {
		dy = -(int)delta * MACOS9_WHEEL_STEP_PX;
	}

	macos9_window_scroll_by(gw, dx, dy);
	return noErr;
}


void
macos9_wheel_install(void)
{
	EventTypeSpec spec;
	EventHandlerUPP upp;
	OSStatus err;

	spec.eventClass = kEventClassMouse;
	spec.eventKind  = kEventMouseWheelMoved;

	upp = NewEventHandlerUPP(macos9_wheel_handler);
	if (upp == NULL)
		return;

	err = InstallApplicationEventHandler(upp, 1, &spec, NULL, NULL);
	/* Log install result. noErr (== 0) is success. Any non-zero
	 * means CarbonLib rejected the registration — wheel events will
	 * still go to the default (crashing) handler chain. */
	macsurf_debug_log_int("whl_i", (long)err);
}

#else /* !__MACOS9__ */

/* Linux cross-check stub. */
void macos9_wheel_install(void)
{
}

#endif /* __MACOS9__ */
