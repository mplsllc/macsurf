const require = (await import('node:module')).createRequire(import.meta.url); const __chunk_filename = (await import('node:url')).fileURLToPath(import.meta.url); const __chunk_dirname = (await import('node:path')).dirname(__chunk_filename);
import {
  require_execAsync
} from "./chunk-5AUYMPVF.js";
import {
  esm_exports,
  init_esm
} from "./chunk-IUUIT4SU.js";
import {
  __commonJS,
  __toCommonJS
} from "./chunk-34MYV7JD.js";

// node_modules/@opentelemetry/resources/build/src/detectors/platform/node/machine-id/getMachineId-darwin.js
var require_getMachineId_darwin = __commonJS({
  "node_modules/@opentelemetry/resources/build/src/detectors/platform/node/machine-id/getMachineId-darwin.js"(exports) {
    Object.defineProperty(exports, "__esModule", { value: true });
    exports.getMachineId = void 0;
    var execAsync_1 = require_execAsync();
    var api_1 = (init_esm(), __toCommonJS(esm_exports));
    async function getMachineId() {
      try {
        const result = await (0, execAsync_1.execAsync)('ioreg -rd1 -c "IOPlatformExpertDevice"');
        const idLine = result.stdout.split("\n").find((line) => line.includes("IOPlatformUUID"));
        if (!idLine) {
          return void 0;
        }
        const parts = idLine.split('" = "');
        if (parts.length === 2) {
          return parts[1].slice(0, -1);
        }
      } catch (e) {
        api_1.diag.debug(`error reading machine id: ${e}`);
      }
      return void 0;
    }
    exports.getMachineId = getMachineId;
  }
});
export default require_getMachineId_darwin();
