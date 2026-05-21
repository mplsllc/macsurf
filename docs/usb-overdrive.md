# Mouse wheel scrolling in MacSurf (USB Overdrive)

**Short answer (current state, as of fixes141):** configure USB Overdrive's
Scroll Wheel action to **"Do Nothing"** when MacSurf is frontmost.
Scrolling is fully available via the scroll bar, keyboard arrow keys,
Page Up / Page Down, and Home / End. The wheel itself is not yet safe
to use on MacSurf, see "Why" below.

## Recommended configuration (interim)

1. Open the **USB Overdrive** control panel.
2. Select MacSurf (or the "Default Settings" if you prefer a global
   rule).
3. In the **Mouse** tab, find the **Scroll Wheel Up** and **Scroll
   Wheel Down** actions.
4. Set both to **"Do Nothing"**.
5. Apply. No restart required.

With this setting, spinning the wheel simply has no effect. Use one
of the other scroll inputs instead.

## Scroll inputs that work today

- Scroll bar drag and thumb track
- Keyboard arrow keys (Up / Down)
- Page Up / Page Down
- Home / End

That is a complete set of scroll inputs for an OS 9 browser even
without the wheel.

## Why the wheel is disabled

Apple's CarbonLib on Mac OS 9 does not carry the
`kEventMouseWheelMoved` Carbon event class, that API was added in
Mac OS X 10.0 and never back-ported (Apple's own `CarbonEvents.h`
marks it `CarbonLib: not available`). MacSurf previously installed a
handler for it (fixes134) which caused an illegal-instruction crash
at `19DBDEB8`; fixes140 removed the handler. After that, spinning the
wheel still occasionally dropped into MacsBug with `Undefined A-Trap
at 1BDC54E0`, a crash in code that was executing from garbage
memory, likely inside CarbonLib's event dispatch or USB Overdrive's
trap patches, not in MacSurf itself. fixes141 added defensive event
filtering (narrowed `WaitNextEvent` mask, whitelist guard in dispatch)
but we cannot currently determine whether the underlying crash still
fires under the new defenses without an ADB keyboard to capture a
real MacsBug stack.

Until that diagnostic hardware is in place, the safe recommendation
is "leave the wheel alone." When the wheel story is re-opened we
will update this doc.

## Earlier recommendation (now superseded)

Prior to fixes141, this doc recommended configuring the wheel to
send **Up Arrow / Down Arrow** key events, which in principle flows
through MacSurf's existing keyboard handler. That may work in
practice but may also still trigger the underlying CarbonLib / USB
Overdrive crash depending on where the bug actually lives. If you
are willing to tolerate occasional crashes to get wheel scrolling,
the arrow-key binding remains a reasonable experiment, but it is no
longer the default recommendation.

## References

- [browser/netsurf/frontends/macos9/macos9_wheel.c](../browser/netsurf/frontends/macos9/macos9_wheel.c)
 , the disabled Carbon handler with full engineering history.
- `CLAUDE.md` § "Mouse Wheel / Input Devices", the authoritative
  internal notes.
- [USB Overdrive](https://www.usboverdrive.com/) ,
  Alessandro Levi Montalcini's USB HID driver for classic and modern
  Mac OS.
