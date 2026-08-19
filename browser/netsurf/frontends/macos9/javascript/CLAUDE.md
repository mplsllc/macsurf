# frontends/macos9/javascript — QuickJS engine glue

`macsurf_qjs.c`/`.h` own `js_initialise`/`js_newheap`/`js_exec`/`js_fire_event` and run
JS natively through `JS_Eval`. `MACSURF_JS_GEOMETRY`, `MACSURF_JS_VIEW_EVENTS`,
`MACSURF_JS_AUDIT`, `MACSURF_JS_MAX_BYTES`, `MACSURF_JS_TIMEOUT_MS` are all defined here
(grep this file for current values — don't trust a cached number from anywhere else, they
move independently of each other).

## XenForo has-js / reply-editor cascade

XenForo's `preamble.min.js` does a hiddenscroll probe that reads `div.parentNode`
immediately after `appendChild`ing the div, then jQuery's Sizzle self-test runs its own
DOM check. If either fails against this engine's native DOM bindings
(`qjs_el_get_parent_node_data`, `qjs_el_append_child_data`, etc.), every script downstream
of `core-compiled.js` throws before it ever finishes setting up `XF.Element` — which
includes the reply editor (Froala) and every XenForo module that depends on it
(`attachment_manager`, `token_input`, `prefix_menu`).

This is the SAME root cause behind "the page looks broken" complaints on the same sites
(blank nav menu, unstyled plain-text reply box): XenForo's theme CSS gates its real
nav/editor styling behind a `.has-js` class on `<html>`, which only gets added by the same
script chain that's throwing. Fixing the native `parentNode`/`appendChild`/`createElement`
binding surface is likely to resolve both the functional and the visual complaint at once
— don't chase the visual symptom as a separate CSS bug.

This binding surface has been rewritten more than once (real DOM mutation, then real
libdom traversal). Re-verify the specific failure mode against the current `macsurf_qjs.c`
before trusting old specifics about exactly where/why `parentNode` comes back wrong.
