const require = (await import('node:module')).createRequire(import.meta.url); const __chunk_filename = (await import('node:url')).fileURLToPath(import.meta.url); const __chunk_dirname = (await import('node:path')).dirname(__chunk_filename);
import {
  __commonJS,
  __require
} from "./chunk-34MYV7JD.js";

// node_modules/@opentelemetry/resources/build/src/detectors/platform/node/machine-id/execAsync.js
var require_execAsync = __commonJS({
  "node_modules/@opentelemetry/resources/build/src/detectors/platform/node/machine-id/execAsync.js"(exports) {
    "use strict";
    Object.defineProperty(exports, "__esModule", { value: true });
    exports.execAsync = void 0;
    var child_process = __require("child_process");
    var util = __require("util");
    exports.execAsync = util.promisify(child_process.exec);
  }
});

export {
  require_execAsync
};
