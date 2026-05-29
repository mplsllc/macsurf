/*
 * mlink_main.c — macLink Faceless Background App entry point
 *
 * Phase 0 deliverable. Validates the FBA pattern under CarbonLib:
 *   - cooperative event loop (WaitNextEvent)
 *   - no menu bar / no front-window
 *   - Open Transport client context initialised
 *   - one loopback listener up on port 8765
 *   - graceful exit on Cmd-Q or system shutdown
 *
 * Success criterion: install in System Folder/Startup Items/, reboot,
 * verify "macLink Debug.log" appears on Desktop with the startup
 * sequence, point any TCP client at 127.0.0.1:8765, observe the
 * ACCEPT line in the log.
 *
 * No TLS yet. No CONNECT parsing yet. No cert minting yet. Those
 * land in Phases 1, 2, 3 respectively.
 *
 * CW8 C89 — no inline, no //, no for-scope decls.
 */
#include <stddef.h>
#include <Events.h>
#include <Quickdraw.h>
#include <AppleEvents.h>

#include "mlink_log.h"
#include "mlink_listener.h"

/* ------------------------------------------------------------------ */
/* Quit-flag, set by AppleEvent handler or by command-key             */
/* ------------------------------------------------------------------ */

static int g_quit = 0;

/* ------------------------------------------------------------------ */
/* AppleEvent: Quit ('quit') handler — required for clean shutdown    */
/* even for an FBA, because the Finder sends 'quit' to all running   */
/* apps at system shutdown.                                          */
/* ------------------------------------------------------------------ */

static pascal OSErr mlink__ae_quit(const AppleEvent *evt,
                                   AppleEvent *reply,
                                   long refcon)
{
    (void)evt;
    (void)reply;
    (void)refcon;
    g_quit = 1;
    mlink_log("ae: quit received");
    return noErr;
}

static OSErr mlink__install_ae_handlers(void)
{
    return AEInstallEventHandler(kCoreEventClass, kAEQuitApplication,
                                 NewAEEventHandlerUPP(mlink__ae_quit),
                                 0, false);
}

/* ------------------------------------------------------------------ */
/* Cooperative event loop                                             */
/* ------------------------------------------------------------------ */

static void mlink__one_iter(void)
{
    EventRecord evt;
    /* Short sleep — keep responsive to OT events. 1 tick = 1/60 s.
     * Listener pump runs every iter regardless. */
    (void)WaitNextEvent(everyEvent, &evt, 6, NULL);

    /* Dispatch the few events we care about. Most we drop on the
     * floor — FBA has no windows. */
    if (evt.what == kHighLevelEvent) {
        (void)AEProcessAppleEvent(&evt);
    }

    mlink_listener_pump();
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* Carbon FBA setup: no Init traps needed (CarbonLib handles them).
     * Mirrors MacSurf's main() preamble. */
    InitCursor();
    FlushEvents(everyEvent, 0);

    /* Diagnostic log first so anything past here gets logged. */
    if (mlink_log_init() != 0) {
        /* No log = no way to communicate from an FBA. Bail. */
        return 1;
    }

    mlink_log("main: macLink starting up");

    if (mlink__install_ae_handlers() != noErr) {
        mlink_log("main: AE handler install FAIL");
        /* Non-fatal — we just won't shut down cleanly. Continue. */
    }

    /* Phase 0: bring up just one listener so we can validate the
     * pattern. Phases 4+ open the rest. */
    if (mlink_listener_start(MLINK_PORT_HTTPS_CONNECT) != 0) {
        mlink_log("main: listener start FAIL — bailing");
        mlink_log_close();
        return 1;
    }

    mlink_log("main: entering event loop");

    while (!g_quit) {
        mlink__one_iter();
    }

    mlink_log("main: shutdown — closing listeners");
    mlink_listener_stop_all();
    mlink_log("main: bye");
    mlink_log_close();

    return 0;
}
