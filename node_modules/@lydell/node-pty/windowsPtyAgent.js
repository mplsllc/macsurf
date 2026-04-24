"use strict";
/**
 * Copyright (c) 2012-2015, Christopher Jeffrey, Peter Sunde (MIT License)
 * Copyright (c) 2016, Daniel Imms (MIT License).
 * Copyright (c) 2018, Microsoft Corporation (MIT License).
 */
Object.defineProperty(exports, "__esModule", { value: true });
var fs = require("fs");
var path = require("path");
var net_1 = require("net");
var child_process_1 = require("child_process");
var windowsConoutConnection_1 = require("./windowsConoutConnection");
var requireBinary_1 = require("./requireBinary");
var conptyNative;
/**
 * The amount of time to wait for additional data after the conpty shell process has exited before
 * shutting down the socket. The timer will be reset if a new data event comes in after the timer
 * has started.
 */
var FLUSH_DATA_INTERVAL = 1000;
/**
 * This agent sits between the WindowsTerminal class and provides an interface for both conpty.
 */
var WindowsPtyAgent = /** @class */ (function () {
    function WindowsPtyAgent(file, args, env, cwd, cols, rows, debug, conptyInheritCursor) {
        var _this = this;
        if (conptyInheritCursor === void 0) { conptyInheritCursor = false; }
        this._pid = 0;
        this._innerPid = 0;
        if (!conptyNative) {
            conptyNative = requireBinary_1.requireBinary('conpty.node');
        }
        this._ptyNative = conptyNative;
        // Sanitize input variable.
        cwd = path.resolve(cwd);
        // Compose command line
        var commandLine = argsToCommandLine(file, args);
        // Open pty session.
        var term = this._ptyNative.startProcess(file, cols, rows, debug, this._generatePipeName(), conptyInheritCursor);
        // Not available on windows.
        this._fd = term.fd;
        // Generated incremental number that has no real purpose besides  using it
        // as a terminal id.
        this._pty = term.pty;
        // Create terminal pipe IPC channel and forward to a local unix socket.
        this._outSocket = new net_1.Socket();
        this._outSocket.setEncoding('utf8');
        // The conout socket must be ready out on another thread to avoid deadlocks
        this._conoutSocketWorker = new windowsConoutConnection_1.ConoutConnection(term.conout);
        this._conoutSocketWorker.onReady(function () {
            _this._conoutSocketWorker.connectSocket(_this._outSocket);
        });
        this._outSocket.on('connect', function () {
            _this._outSocket.emit('ready_datapipe');
        });
        var inSocketFD = fs.openSync(term.conin, 'w');
        this._inSocket = new net_1.Socket({
            fd: inSocketFD,
            readable: false,
            writable: true
        });
        this._inSocket.setEncoding('utf8');
        var connect = this._ptyNative.connect(this._pty, commandLine, cwd, env, function (c) { return _this._$onProcessExit(c); });
        this._innerPid = connect.pid;
    }
    Object.defineProperty(WindowsPtyAgent.prototype, "inSocket", {
        get: function () { return this._inSocket; },
        enumerable: true,
        configurable: true
    });
    Object.defineProperty(WindowsPtyAgent.prototype, "outSocket", {
        get: function () { return this._outSocket; },
        enumerable: true,
        configurable: true
    });
    Object.defineProperty(WindowsPtyAgent.prototype, "fd", {
        get: function () { return this._fd; },
        enumerable: true,
        configurable: true
    });
    Object.defineProperty(WindowsPtyAgent.prototype, "innerPid", {
        get: function () { return this._innerPid; },
        enumerable: true,
        configurable: true
    });
    Object.defineProperty(WindowsPtyAgent.prototype, "pty", {
        get: function () { return this._pty; },
        enumerable: true,
        configurable: true
    });
    WindowsPtyAgent.prototype.resize = function (cols, rows) {
        if (this._exitCode !== undefined) {
            throw new Error('Cannot resize a pty that has already exited');
        }
        this._ptyNative.resize(this._pty, cols, rows);
    };
    WindowsPtyAgent.prototype.clear = function () {
        this._ptyNative.clear(this._pty);
    };
    WindowsPtyAgent.prototype.kill = function () {
        var _this = this;
        this._inSocket.readable = false;
        this._outSocket.readable = false;
        // Tell the agent to kill the pty, this releases handles to the process
        this._getConsoleProcessList().then(function (consoleProcessList) {
            consoleProcessList.forEach(function (pid) {
                try {
                    process.kill(pid);
                }
                catch (e) {
                    // Ignore if process cannot be found (kill ESRCH error)
                }
            });
            _this._ptyNative.kill(_this._pty);
        });
        this._conoutSocketWorker.dispose();
    };
    WindowsPtyAgent.prototype._getConsoleProcessList = function () {
        var _this = this;
        return new Promise(function (resolve) {
            var agent = child_process_1.fork(path.join(__dirname, 'conpty_console_list_agent'), [_this._innerPid.toString()]);
            agent.on('message', function (message) {
                clearTimeout(timeout);
                resolve(message.consoleProcessList);
            });
            var timeout = setTimeout(function () {
                // Something went wrong, just send back the shell PID
                agent.kill();
                resolve([_this._innerPid]);
            }, 5000);
        });
    };
    Object.defineProperty(WindowsPtyAgent.prototype, "exitCode", {
        get: function () {
            return this._exitCode;
        },
        enumerable: true,
        configurable: true
    });
    WindowsPtyAgent.prototype._generatePipeName = function () {
        return "conpty-" + Math.random() * 10000000;
    };
    /**
     * Triggered from the native side when a contpy process exits.
     */
    WindowsPtyAgent.prototype._$onProcessExit = function (exitCode) {
        var _this = this;
        this._exitCode = exitCode;
        this._flushDataAndCleanUp();
        this._outSocket.on('data', function () { return _this._flushDataAndCleanUp(); });
    };
    WindowsPtyAgent.prototype._flushDataAndCleanUp = function () {
        var _this = this;
        if (this._closeTimeout) {
            clearTimeout(this._closeTimeout);
        }
        this._closeTimeout = setTimeout(function () { return _this._cleanUpProcess(); }, FLUSH_DATA_INTERVAL);
    };
    WindowsPtyAgent.prototype._cleanUpProcess = function () {
        this._inSocket.readable = false;
        this._outSocket.readable = false;
        this._outSocket.destroy();
    };
    return WindowsPtyAgent;
}());
exports.WindowsPtyAgent = WindowsPtyAgent;
// Convert argc/argv into a Win32 command-line following the escaping convention
// documented on MSDN (e.g. see CommandLineToArgvW documentation). Copied from
// winpty project.
function argsToCommandLine(file, args) {
    if (isCommandLine(args)) {
        if (args.length === 0) {
            return file;
        }
        return argsToCommandLine(file, []) + " " + args;
    }
    var argv = [file];
    Array.prototype.push.apply(argv, args);
    var result = '';
    for (var argIndex = 0; argIndex < argv.length; argIndex++) {
        if (argIndex > 0) {
            result += ' ';
        }
        var arg = argv[argIndex];
        // if it is empty or it contains whitespace and is not already quoted
        var hasLopsidedEnclosingQuote = xOr((arg[0] !== '"'), (arg[arg.length - 1] !== '"'));
        var hasNoEnclosingQuotes = ((arg[0] !== '"') && (arg[arg.length - 1] !== '"'));
        var quote = arg === '' ||
            (arg.indexOf(' ') !== -1 ||
                arg.indexOf('\t') !== -1) &&
                ((arg.length > 1) &&
                    (hasLopsidedEnclosingQuote || hasNoEnclosingQuotes));
        if (quote) {
            result += '\"';
        }
        var bsCount = 0;
        for (var i = 0; i < arg.length; i++) {
            var p = arg[i];
            if (p === '\\') {
                bsCount++;
            }
            else if (p === '"') {
                result += repeatText('\\', bsCount * 2 + 1);
                result += '"';
                bsCount = 0;
            }
            else {
                result += repeatText('\\', bsCount);
                bsCount = 0;
                result += p;
            }
        }
        if (quote) {
            result += repeatText('\\', bsCount * 2);
            result += '\"';
        }
        else {
            result += repeatText('\\', bsCount);
        }
    }
    return result;
}
exports.argsToCommandLine = argsToCommandLine;
function isCommandLine(args) {
    return typeof args === 'string';
}
function repeatText(text, count) {
    var result = '';
    for (var i = 0; i < count; i++) {
        result += text;
    }
    return result;
}
function xOr(arg1, arg2) {
    return ((arg1 && !arg2) || (!arg1 && arg2));
}
