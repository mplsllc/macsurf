"use strict";
var __extends = (this && this.__extends) || (function () {
    var extendStatics = function (d, b) {
        extendStatics = Object.setPrototypeOf ||
            ({ __proto__: [] } instanceof Array && function (d, b) { d.__proto__ = b; }) ||
            function (d, b) { for (var p in b) if (b.hasOwnProperty(p)) d[p] = b[p]; };
        return extendStatics(d, b);
    };
    return function (d, b) {
        extendStatics(d, b);
        function __() { this.constructor = d; }
        d.prototype = b === null ? Object.create(b) : (__.prototype = b.prototype, new __());
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
var tty = require("tty");
var terminal_1 = require("./terminal");
var utils_1 = require("./utils");
var requireBinary_1 = require("./requireBinary");
var pty = requireBinary_1.requireBinary('pty.node');
var helperPath = "@lydell/node-pty-" + process.platform + "-" + process.arch + "/spawn-helper";
helperPath = process.platform === 'darwin' ? require.resolve(helperPath) : 'spawn-helper-unused';
helperPath = helperPath.replace('app.asar', 'app.asar.unpacked');
helperPath = helperPath.replace('node_modules.asar', 'node_modules.asar.unpacked');
var DEFAULT_FILE = 'sh';
var DEFAULT_NAME = 'xterm';
var DESTROY_SOCKET_TIMEOUT_MS = 200;
var UnixTerminal = /** @class */ (function (_super) {
    __extends(UnixTerminal, _super);
    function UnixTerminal(file, args, opt) {
        var _a, _b;
        var _this = _super.call(this, opt) || this;
        _this._boundClose = false;
        _this._emittedClose = false;
        if (typeof args === 'string') {
            throw new Error('args as a string is not supported on unix.');
        }
        // Initialize arguments
        args = args || [];
        file = file || DEFAULT_FILE;
        opt = opt || {};
        opt.env = opt.env || process.env;
        _this._cols = opt.cols || terminal_1.DEFAULT_COLS;
        _this._rows = opt.rows || terminal_1.DEFAULT_ROWS;
        var uid = (_a = opt.uid) !== null && _a !== void 0 ? _a : -1;
        var gid = (_b = opt.gid) !== null && _b !== void 0 ? _b : -1;
        var env = utils_1.assign({}, opt.env);
        if (opt.env === process.env) {
            _this._sanitizeEnv(env);
        }
        var cwd = opt.cwd || process.cwd();
        env.PWD = cwd;
        var name = opt.name || env.TERM || DEFAULT_NAME;
        env.TERM = name;
        var parsedEnv = _this._parseEnv(env);
        var encoding = (opt.encoding === undefined ? 'utf8' : opt.encoding);
        var onexit = function (code, signal) {
            // XXX Sometimes a data event is emitted after exit. Wait til socket is
            // destroyed.
            if (!_this._emittedClose) {
                if (_this._boundClose) {
                    return;
                }
                _this._boundClose = true;
                // From macOS High Sierra 10.13.2 sometimes the socket never gets
                // closed. A timeout is applied here to avoid the terminal never being
                // destroyed when this occurs.
                var timeout_1 = setTimeout(function () {
                    timeout_1 = null;
                    // Destroying the socket now will cause the close event to fire
                    _this._socket.destroy();
                }, DESTROY_SOCKET_TIMEOUT_MS);
                _this.once('close', function () {
                    if (timeout_1 !== null) {
                        clearTimeout(timeout_1);
                    }
                    _this.emit('exit', code, signal);
                });
                return;
            }
            _this.emit('exit', code, signal);
        };
        // fork
        var term = pty.fork(file, args, parsedEnv, cwd, _this._cols, _this._rows, uid, gid, (encoding === 'utf8'), helperPath, onexit);
        _this._socket = new tty.ReadStream(term.fd);
        if (encoding !== null) {
            _this._socket.setEncoding(encoding);
        }
        // setup
        _this._socket.on('error', function (err) {
            // NOTE: fs.ReadStream gets EAGAIN twice at first:
            if (err.code) {
                if (~err.code.indexOf('EAGAIN')) {
                    return;
                }
            }
            // close
            _this._close();
            // EIO on exit from fs.ReadStream:
            if (!_this._emittedClose) {
                _this._emittedClose = true;
                _this.emit('close');
            }
            // EIO, happens when someone closes our child process: the only process in
            // the terminal.
            // node < 0.6.14: errno 5
            // node >= 0.6.14: read EIO
            if (err.code) {
                if (~err.code.indexOf('errno 5') || ~err.code.indexOf('EIO')) {
                    return;
                }
            }
            // throw anything else
            if (_this.listeners('error').length < 2) {
                throw err;
            }
        });
        _this._pid = term.pid;
        _this._fd = term.fd;
        _this._pty = term.pty;
        _this._file = file;
        _this._name = name;
        _this._readable = true;
        _this._writable = true;
        _this._socket.on('close', function () {
            if (_this._emittedClose) {
                return;
            }
            _this._emittedClose = true;
            _this._close();
            _this.emit('close');
        });
        _this._forwardEvents();
        return _this;
    }
    Object.defineProperty(UnixTerminal.prototype, "master", {
        get: function () { return this._master; },
        enumerable: true,
        configurable: true
    });
    Object.defineProperty(UnixTerminal.prototype, "slave", {
        get: function () { return this._slave; },
        enumerable: true,
        configurable: true
    });
    UnixTerminal.prototype._write = function (data) {
        this._socket.write(data);
    };
    Object.defineProperty(UnixTerminal.prototype, "fd", {
        /* Accessors */
        get: function () { return this._fd; },
        enumerable: true,
        configurable: true
    });
    Object.defineProperty(UnixTerminal.prototype, "ptsName", {
        get: function () { return this._pty; },
        enumerable: true,
        configurable: true
    });
    /**
     * openpty
     */
    UnixTerminal.open = function (opt) {
        var self = Object.create(UnixTerminal.prototype);
        opt = opt || {};
        if (arguments.length > 1) {
            opt = {
                cols: arguments[1],
                rows: arguments[2]
            };
        }
        var cols = opt.cols || terminal_1.DEFAULT_COLS;
        var rows = opt.rows || terminal_1.DEFAULT_ROWS;
        var encoding = (opt.encoding === undefined ? 'utf8' : opt.encoding);
        // open
        var term = pty.open(cols, rows);
        self._master = new tty.ReadStream(term.master);
        if (encoding !== null) {
            self._master.setEncoding(encoding);
        }
        self._master.resume();
        self._slave = new tty.ReadStream(term.slave);
        if (encoding !== null) {
            self._slave.setEncoding(encoding);
        }
        self._slave.resume();
        self._socket = self._master;
        self._pid = -1;
        self._fd = term.master;
        self._pty = term.pty;
        self._file = process.argv[0] || 'node';
        self._name = process.env.TERM || '';
        self._readable = true;
        self._writable = true;
        self._socket.on('error', function (err) {
            self._close();
            if (self.listeners('error').length < 2) {
                throw err;
            }
        });
        self._socket.on('close', function () {
            self._close();
        });
        return self;
    };
    UnixTerminal.prototype.destroy = function () {
        var _this = this;
        this._close();
        // Need to close the read stream so node stops reading a dead file
        // descriptor. Then we can safely SIGHUP the shell.
        this._socket.once('close', function () {
            _this.kill('SIGHUP');
        });
        this._socket.destroy();
    };
    UnixTerminal.prototype.kill = function (signal) {
        try {
            process.kill(this.pid, signal || 'SIGHUP');
        }
        catch (e) { /* swallow */ }
    };
    Object.defineProperty(UnixTerminal.prototype, "process", {
        /**
         * Gets the name of the process.
         */
        get: function () {
            if (process.platform === 'darwin') {
                var title = pty.process(this._fd);
                return (title !== 'kernel_task') ? title : this._file;
            }
            return pty.process(this._fd, this._pty) || this._file;
        },
        enumerable: true,
        configurable: true
    });
    /**
     * TTY
     */
    UnixTerminal.prototype.resize = function (cols, rows) {
        if (cols <= 0 || rows <= 0 || isNaN(cols) || isNaN(rows) || cols === Infinity || rows === Infinity) {
            throw new Error('resizing must be done using positive cols and rows');
        }
        pty.resize(this._fd, cols, rows);
        this._cols = cols;
        this._rows = rows;
    };
    UnixTerminal.prototype.clear = function () {
    };
    UnixTerminal.prototype._sanitizeEnv = function (env) {
        // Make sure we didn't start our server from inside tmux.
        delete env['TMUX'];
        delete env['TMUX_PANE'];
        // Make sure we didn't start our server from inside screen.
        // http://web.mit.edu/gnu/doc/html/screen_20.html
        delete env['STY'];
        delete env['WINDOW'];
        // Delete some variables that might confuse our terminal.
        delete env['WINDOWID'];
        delete env['TERMCAP'];
        delete env['COLUMNS'];
        delete env['LINES'];
    };
    return UnixTerminal;
}(terminal_1.Terminal));
exports.UnixTerminal = UnixTerminal;
