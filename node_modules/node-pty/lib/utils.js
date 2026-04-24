"use strict";
/**
 * Copyright (c) 2017, Daniel Imms (MIT License).
 * Copyright (c) 2018, Microsoft Corporation (MIT License).
 */
Object.defineProperty(exports, "__esModule", { value: true });
exports.loadNativeModule = exports.assign = void 0;
function assign(target) {
    var sources = [];
    for (var _i = 1; _i < arguments.length; _i++) {
        sources[_i - 1] = arguments[_i];
    }
    sources.forEach(function (source) { return Object.keys(source).forEach(function (key) { return target[key] = source[key]; }); });
    return target;
}
exports.assign = assign;
function loadNativeModule(name) {
    // Check build, debug, and then prebuilds.
    var dirs = ['build/Release', 'build/Debug', "prebuilds/" + process.platform + "-" + process.arch];
    // Check relative to the parent dir for unbundled and then the current dir for bundled
    var relative = ['..', '.'];
    var lastError;
    for (var _i = 0, dirs_1 = dirs; _i < dirs_1.length; _i++) {
        var d = dirs_1[_i];
        for (var _a = 0, relative_1 = relative; _a < relative_1.length; _a++) {
            var r = relative_1[_a];
            var dir = r + "/" + d + "/";
            try {
                return { dir: dir, module: require(dir + "/" + name + ".node") };
            }
            catch (e) {
                lastError = e;
            }
        }
    }
    throw new Error("Failed to load native module: " + name + ".node, checked: " + dirs.join(', ') + ": " + lastError);
}
exports.loadNativeModule = loadNativeModule;
//# sourceMappingURL=utils.js.map