const require = (await import('node:module')).createRequire(import.meta.url); const __chunk_filename = (await import('node:url')).fileURLToPath(import.meta.url); const __chunk_dirname = (await import('node:path')).dirname(__chunk_filename);
import {
  esm_exports,
  init_esm
} from "./chunk-IUUIT4SU.js";
import {
  __commonJS,
  __toCommonJS
} from "./chunk-34MYV7JD.js";

// node_modules/@opentelemetry/resources/build/src/detectors/platform/node/machine-id/getMachineId-unsupported.js
var require_getMachineId_unsupported = __commonJS({
  "node_modules/@opentelemetry/resources/build/src/detectors/platform/node/machine-id/getMachineId-unsupported.js"(exports) {
    Object.defineProperty(exports, "__esModule", { value: true });
    exports.getMachineId = void 0;
    var api_1 = (init_esm(), __toCommonJS(esm_exports));
    async function getMachineId() {
      api_1.diag.debug("could not read machine-id: unsupported platform");
      return void 0;
    }
    exports.getMachineId = getMachineId;
  }
});
export default require_getMachineId_unsupported();
