const require = (await import('node:module')).createRequire(import.meta.url); const __chunk_filename = (await import('node:url')).fileURLToPath(import.meta.url); const __chunk_dirname = (await import('node:path')).dirname(__chunk_filename);

// packages/cli/src/utils/events.ts
import { EventEmitter } from "node:events";
var TransientMessageType = /* @__PURE__ */ ((TransientMessageType2) => {
  TransientMessageType2["Warning"] = "warning";
  TransientMessageType2["Hint"] = "hint";
  return TransientMessageType2;
})(TransientMessageType || {});
var AppEvent = /* @__PURE__ */ ((AppEvent2) => {
  AppEvent2["OpenDebugConsole"] = "open-debug-console";
  AppEvent2["Flicker"] = "flicker";
  AppEvent2["SelectionWarning"] = "selection-warning";
  AppEvent2["PasteTimeout"] = "paste-timeout";
  AppEvent2["TerminalBackground"] = "terminal-background";
  AppEvent2["TransientMessage"] = "transient-message";
  AppEvent2["ScrollToBottom"] = "scroll-to-bottom";
  return AppEvent2;
})(AppEvent || {});
var appEvents = new EventEmitter();

export {
  TransientMessageType,
  AppEvent,
  appEvents
};
/**
 * @license
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */
