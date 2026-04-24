"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
var PACKAGE_NAME = "@lydell/node-pty-" + process.platform + "-" + process.arch;
var help = "\nThis can happen if you use the \"--omit=optional\" (or \"--no-optional\") npm flag.\nThe \"optionalDependencies\" package.json feature is used to install the correct\nbinary executable for your current platform. Remove that flag to use @lydell/node-pty.\n\nThis can also happen if the \"node_modules\" folder was copied between two operating systems\nthat need different binaries - including \"virtual\" operating systems like Docker and WSL.\nIf so, try installing with npm rather than copying \"node_modules\".\n".trim();
function requireBinary(file) {
    try {
        return require(PACKAGE_NAME + "/" + file);
    }
    catch (error) {
        if (error && error.code === 'MODULE_NOT_FOUND') {
            var optionalDependencies = getOptionalDependencies();
            throw new Error(optionalDependencies === undefined
                ? "The @lydell/node-pty package could not find the binary package: " + PACKAGE_NAME + "/" + file + "\n\n" + help + "\n\nYour platform (" + process.platform + "-" + process.arch + ") might not be supported."
                : PACKAGE_NAME in optionalDependencies
                    ? "The @lydell/node-pty package supports your platform (" + process.platform + "-" + process.arch + "), but it could not find the binary package for it: " + PACKAGE_NAME + "/" + file + "\n\n" + help
                    : "The @lydell/node-pty package currently does not support your platform: " + process.platform + "-" + process.arch, 
            // @ts-ignore: The TypeScript setup does not seem to support cause.
            { cause: error });
        }
        else {
            throw error;
        }
    }
}
exports.requireBinary = requireBinary;
function getOptionalDependencies() {
    try {
        return require('./package.json').optionalDependencies;
    }
    catch (_error) {
        return undefined;
    }
}
