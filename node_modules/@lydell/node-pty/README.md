# @lydell/node-pty

Fork of [microsoft/node-pty](https://github.com/microsoft/node-pty) with prebuilt binaries.

- Only supports platforms with prebuilt binaries. (Never builds with node-gyp at install.)
- Removed support for old versions of Windows. (Removed winpty, only supports conpty.)

## pty.js

microsoft/node-pty is forked from [chjj/pty.js](https://github.com/chjj/pty.js) with the primary goals being to provide better support for later Node.js versions and Windows.

## License

Copyright (c) 2012-2015, Christopher Jeffrey (MIT License).<br>
Copyright (c) 2016, Daniel Imms (MIT License).<br>
Copyright (c) 2018, Microsoft Corporation (MIT License).

## Version

@lydell/node-pty@1.1.0 is based on node-pty@1.1.0-beta14 (commit [efbf8ebbe7b09351548fa0a4ed07b36c5d2e539b](https://github.com/microsoft/node-pty/commit/efbf8ebbe7b09351548fa0a4ed07b36c5d2e539b)).

## Prebuilt binaries

This package includes prebuilt binaries for the following platforms and architectures:

- macOS x86_64 (darwin-x64)
- macOS ARM64 (darwin-arm64)
- Linux x86_64 (linux-x64)
- Linux ARM64 (linux-arm64) (not tested)
- Windows x86_64 (win32-x64)
- Windows ARM64 (win32-arm64) (not tested)