"use strict";
/**
 * Copyright (c) 2017, Daniel Imms (MIT License).
 * Copyright (c) 2018, Microsoft Corporation (MIT License).
 */
var __awaiter = (this && this.__awaiter) || function (thisArg, _arguments, P, generator) {
    function adopt(value) { return value instanceof P ? value : new P(function (resolve) { resolve(value); }); }
    return new (P || (P = Promise))(function (resolve, reject) {
        function fulfilled(value) { try { step(generator.next(value)); } catch (e) { reject(e); } }
        function rejected(value) { try { step(generator["throw"](value)); } catch (e) { reject(e); } }
        function step(result) { result.done ? resolve(result.value) : adopt(result.value).then(fulfilled, rejected); }
        step((generator = generator.apply(thisArg, _arguments || [])).next());
    });
};
var __generator = (this && this.__generator) || function (thisArg, body) {
    var _ = { label: 0, sent: function() { if (t[0] & 1) throw t[1]; return t[1]; }, trys: [], ops: [] }, f, y, t, g;
    return g = { next: verb(0), "throw": verb(1), "return": verb(2) }, typeof Symbol === "function" && (g[Symbol.iterator] = function() { return this; }), g;
    function verb(n) { return function (v) { return step([n, v]); }; }
    function step(op) {
        if (f) throw new TypeError("Generator is already executing.");
        while (_) try {
            if (f = 1, y && (t = op[0] & 2 ? y["return"] : op[0] ? y["throw"] || ((t = y["return"]) && t.call(y), 0) : y.next) && !(t = t.call(y, op[1])).done) return t;
            if (y = 0, t) op = [op[0] & 2, t.value];
            switch (op[0]) {
                case 0: case 1: t = op; break;
                case 4: _.label++; return { value: op[1], done: false };
                case 5: _.label++; y = op[1]; op = [0]; continue;
                case 7: op = _.ops.pop(); _.trys.pop(); continue;
                default:
                    if (!(t = _.trys, t = t.length > 0 && t[t.length - 1]) && (op[0] === 6 || op[0] === 2)) { _ = 0; continue; }
                    if (op[0] === 3 && (!t || (op[1] > t[0] && op[1] < t[3]))) { _.label = op[1]; break; }
                    if (op[0] === 6 && _.label < t[1]) { _.label = t[1]; t = op; break; }
                    if (t && _.label < t[2]) { _.label = t[2]; _.ops.push(op); break; }
                    if (t[2]) _.ops.pop();
                    _.trys.pop(); continue;
            }
            op = body.call(thisArg, _);
        } catch (e) { op = [6, e]; y = 0; } finally { f = t = 0; }
        if (op[0] & 5) throw op[1]; return { value: op[0] ? op[1] : void 0, done: true };
    }
};
Object.defineProperty(exports, "__esModule", { value: true });
var unixTerminal_1 = require("./unixTerminal");
var assert = require("assert");
var cp = require("child_process");
var path = require("path");
var tty = require("tty");
var fs = require("fs");
var os_1 = require("os");
var testUtils_test_1 = require("./testUtils.test");
var FIXTURES_PATH = path.normalize(path.join(__dirname, '..', 'fixtures', 'utf8-character.txt'));
if (process.platform !== 'win32') {
    describe('UnixTerminal', function () {
        describe('Constructor', function () {
            it('should set a valid pts name', function () {
                var term = new unixTerminal_1.UnixTerminal('/bin/bash', [], {});
                var regExp;
                if (process.platform === 'linux') {
                    // https://linux.die.net/man/4/pts
                    regExp = /^\/dev\/pts\/\d+$/;
                }
                if (process.platform === 'darwin') {
                    // https://developer.apple.com/legacy/library/documentation/Darwin/Reference/ManPages/man4/pty.4.html
                    regExp = /^\/dev\/tty[p-sP-S][a-z0-9]+$/;
                }
                if (regExp) {
                    assert.ok(regExp.test(term.ptsName), '"' + term.ptsName + '" should match ' + regExp.toString());
                }
                assert.ok(tty.isatty(term.fd));
            });
        });
        describe('PtyForkEncodingOption', function () {
            it('should default to utf8', function (done) {
                var term = new unixTerminal_1.UnixTerminal('/bin/bash', ['-c', "cat \"" + FIXTURES_PATH + "\""]);
                term.on('data', function (data) {
                    assert.strictEqual(typeof data, 'string');
                    assert.strictEqual(data, '\u00E6');
                    done();
                });
            });
            it('should return a Buffer when encoding is null', function (done) {
                var term = new unixTerminal_1.UnixTerminal('/bin/bash', ['-c', "cat \"" + FIXTURES_PATH + "\""], {
                    encoding: null
                });
                term.on('data', function (data) {
                    assert.strictEqual(typeof data, 'object');
                    assert.ok(data instanceof Buffer);
                    assert.strictEqual(0xC3, data[0]);
                    assert.strictEqual(0xA6, data[1]);
                    done();
                });
            });
            it('should support other encodings', function (done) {
                var text = 'test æ!';
                var term = new unixTerminal_1.UnixTerminal(undefined, ['-c', 'echo "' + text + '"'], {
                    encoding: 'base64'
                });
                var buffer = '';
                term.onData(function (data) {
                    assert.strictEqual(typeof data, 'string');
                    buffer += data;
                });
                term.onExit(function () {
                    assert.strictEqual(Buffer.alloc(8, buffer, 'base64').toString().replace('\r', '').replace('\n', ''), text);
                    done();
                });
            });
        });
        describe('open', function () {
            var term;
            afterEach(function () {
                if (term) {
                    term.slave.destroy();
                    term.master.destroy();
                }
            });
            it('should open a pty with access to a master and slave socket', function (done) {
                term = unixTerminal_1.UnixTerminal.open({});
                var slavebuf = '';
                term.slave.on('data', function (data) {
                    slavebuf += data;
                });
                var masterbuf = '';
                term.master.on('data', function (data) {
                    masterbuf += data;
                });
                testUtils_test_1.pollUntil(function () {
                    if (masterbuf === 'slave\r\nmaster\r\n' && slavebuf === 'master\n') {
                        done();
                        return true;
                    }
                    return false;
                }, 200, 10);
                term.slave.write('slave\n');
                term.master.write('master\n');
            });
        });
        describe('close', function () {
            var term = new unixTerminal_1.UnixTerminal('node');
            it('should exit when terminal is destroyed programmatically', function (done) {
                term.on('exit', function (code, signal) {
                    assert.strictEqual(code, 0);
                    assert.strictEqual(signal, os_1.constants.signals.SIGHUP);
                    done();
                });
                term.destroy();
            });
        });
        describe('signals in parent and child', function () {
            it('SIGINT - custom in parent and child', function (done) {
                // this test is cumbersome - we have to run it in a sub process to
                // see behavior of SIGINT handlers
                var data = "\n        var pty = require('./lib/index');\n        process.on('SIGINT', () => console.log('SIGINT in parent'));\n        var ptyProcess = pty.spawn('node', ['-e', 'process.on(\"SIGINT\", ()=>console.log(\"SIGINT in child\"));setTimeout(() => null, 300);'], {\n          name: 'xterm-color',\n          cols: 80,\n          rows: 30,\n          cwd: process.env.HOME,\n          env: process.env\n        });\n        ptyProcess.on('data', function (data) {\n          console.log(data);\n        });\n        setTimeout(() => null, 500);\n        console.log('ready', ptyProcess.pid);\n        ";
                var buffer = [];
                var p = cp.spawn('node', ['-e', data]);
                var sub = '';
                p.stdout.on('data', function (data) {
                    if (!data.toString().indexOf('ready')) {
                        sub = data.toString().split(' ')[1].slice(0, -1);
                        setTimeout(function () {
                            process.kill(parseInt(sub), 'SIGINT'); // SIGINT to child
                            p.kill('SIGINT'); // SIGINT to parent
                        }, 200);
                    }
                    else {
                        buffer.push(data.toString().replace(/^\s+|\s+$/g, ''));
                    }
                });
                p.on('close', function () {
                    // handlers in parent and child should have been triggered
                    assert.strictEqual(buffer.indexOf('SIGINT in child') !== -1, true);
                    assert.strictEqual(buffer.indexOf('SIGINT in parent') !== -1, true);
                    done();
                });
            });
            it('SIGINT - custom in parent, default in child', function (done) {
                // this tests the original idea of the signal(...) change in pty.cc:
                // to make sure the SIGINT handler of a pty child is reset to default
                // and does not interfere with the handler in the parent
                var data = "\n        var pty = require('./lib/index');\n        process.on('SIGINT', () => console.log('SIGINT in parent'));\n        var ptyProcess = pty.spawn('node', ['-e', 'setTimeout(() => console.log(\"should not be printed\"), 300);'], {\n          name: 'xterm-color',\n          cols: 80,\n          rows: 30,\n          cwd: process.env.HOME,\n          env: process.env\n        });\n        ptyProcess.on('data', function (data) {\n          console.log(data);\n        });\n        setTimeout(() => null, 500);\n        console.log('ready', ptyProcess.pid);\n        ";
                var buffer = [];
                var p = cp.spawn('node', ['-e', data]);
                var sub = '';
                p.stdout.on('data', function (data) {
                    if (!data.toString().indexOf('ready')) {
                        sub = data.toString().split(' ')[1].slice(0, -1);
                        setTimeout(function () {
                            process.kill(parseInt(sub), 'SIGINT'); // SIGINT to child
                            p.kill('SIGINT'); // SIGINT to parent
                        }, 200);
                    }
                    else {
                        buffer.push(data.toString().replace(/^\s+|\s+$/g, ''));
                    }
                });
                p.on('close', function () {
                    // handlers in parent and child should have been triggered
                    assert.strictEqual(buffer.indexOf('should not be printed') !== -1, false);
                    assert.strictEqual(buffer.indexOf('SIGINT in parent') !== -1, true);
                    done();
                });
            });
            it('SIGHUP default (child only)', function (done) {
                var term = new unixTerminal_1.UnixTerminal('node', ['-e', "\n        console.log('ready');\n        setTimeout(()=>console.log('timeout'), 200);"
                ]);
                var buffer = '';
                term.on('data', function (data) {
                    if (data === 'ready\r\n') {
                        term.kill();
                    }
                    else {
                        buffer += data;
                    }
                });
                term.on('exit', function () {
                    // no timeout in buffer
                    assert.strictEqual(buffer, '');
                    done();
                });
            });
            it('SIGUSR1 - custom in parent and child', function (done) {
                var pHandlerCalled = 0;
                var handleSigUsr = function (h) {
                    return function () {
                        pHandlerCalled += 1;
                        process.removeListener('SIGUSR1', h);
                    };
                };
                process.on('SIGUSR1', handleSigUsr(handleSigUsr));
                var term = new unixTerminal_1.UnixTerminal('node', ['-e', "\n        process.on('SIGUSR1', () => {\n          console.log('SIGUSR1 in child');\n        });\n        console.log('ready');\n        setTimeout(()=>null, 200);"
                ]);
                var buffer = '';
                term.on('data', function (data) {
                    if (data === 'ready\r\n') {
                        process.kill(process.pid, 'SIGUSR1');
                        term.kill('SIGUSR1');
                    }
                    else {
                        buffer += data;
                    }
                });
                term.on('exit', function () {
                    // should have called both handlers and only once
                    assert.strictEqual(pHandlerCalled, 1);
                    assert.strictEqual(buffer, 'SIGUSR1 in child\r\n');
                    done();
                });
            });
        });
        describe('spawn', function () {
            if (process.platform === 'darwin') {
                it('should return the name of the process', function (done) {
                    var term = new unixTerminal_1.UnixTerminal('/bin/echo');
                    assert.strictEqual(term.process, '/bin/echo');
                    term.on('exit', function () { return done(); });
                    term.destroy();
                });
                it('should return the name of the sub process', function (done) {
                    var data = "\n          var pty = require('./lib/index');\n          var ptyProcess = pty.spawn('zsh', ['-c', 'python3'], {\n            env: process.env\n          });\n          ptyProcess.on('data', function (data) {\n            if (ptyProcess.process === 'Python') {\n              console.log('title', ptyProcess.process);\n              console.log('ready', ptyProcess.pid);\n            }\n          });\n          ";
                    var p = cp.spawn('node', ['-e', data]);
                    var sub = '';
                    var pid = '';
                    p.stdout.on('data', function (data) {
                        if (!data.toString().indexOf('title')) {
                            sub = data.toString().split(' ')[1].slice(0, -1);
                        }
                        else if (!data.toString().indexOf('ready')) {
                            pid = data.toString().split(' ')[1].slice(0, -1);
                            process.kill(parseInt(pid), 'SIGINT');
                            p.kill('SIGINT');
                        }
                    });
                    p.on('exit', function () {
                        assert.notStrictEqual(pid, '');
                        assert.strictEqual(sub, 'Python');
                        done();
                    });
                });
                it('should close on exec', function (done) {
                    var data = "\n          var pty = require('./lib/index');\n          var ptyProcess = pty.spawn('node', ['-e', 'setTimeout(() => console.log(\"hello from terminal\"), 300);']);\n          ptyProcess.on('data', function (data) {\n            console.log(data);\n          });\n          setTimeout(() => null, 500);\n          console.log('ready', ptyProcess.pid);\n          ";
                    var buffer = [];
                    var readFd = fs.openSync(FIXTURES_PATH, 'r');
                    var p = cp.spawn('node', ['-e', data], {
                        stdio: ['ignore', 'pipe', 'pipe', readFd]
                    });
                    var sub = '';
                    p.stdout.on('data', function (data) {
                        if (!data.toString().indexOf('ready')) {
                            sub = data.toString().split(' ')[1].slice(0, -1);
                            try {
                                fs.statSync("/proc/" + sub + "/fd/" + readFd);
                                done('not reachable');
                            }
                            catch (error) {
                                assert.notStrictEqual(error.message.indexOf('ENOENT'), -1);
                            }
                            setTimeout(function () {
                                process.kill(parseInt(sub), 'SIGINT'); // SIGINT to child
                                p.kill('SIGINT'); // SIGINT to parent
                            }, 200);
                        }
                        else {
                            buffer.push(data.toString().replace(/^\s+|\s+$/g, ''));
                        }
                    });
                    p.on('close', function () {
                        done();
                    });
                });
            }
            it('should handle exec() errors', function (done) {
                var term = new unixTerminal_1.UnixTerminal('/bin/bogus.exe', []);
                term.on('exit', function (code, signal) {
                    assert.strictEqual(code, 1);
                    done();
                });
            });
            it('should handle chdir() errors', function (done) {
                var term = new unixTerminal_1.UnixTerminal('/bin/echo', [], { cwd: '/nowhere' });
                term.on('exit', function (code, signal) {
                    assert.strictEqual(code, 1);
                    done();
                });
            });
            it('should not leak child process', function (done) {
                var count = cp.execSync('ps -ax | grep node | wc -l');
                var term = new unixTerminal_1.UnixTerminal('node', ['-e', "\n          console.log('ready');\n          setTimeout(()=>console.log('timeout'), 200);"
                ]);
                term.on('data', function (data) { return __awaiter(void 0, void 0, void 0, function () {
                    var newCount;
                    return __generator(this, function (_a) {
                        switch (_a.label) {
                            case 0:
                                if (!(data === 'ready\r\n')) return [3 /*break*/, 2];
                                process.kill(term.pid, 'SIGINT');
                                return [4 /*yield*/, setTimeout(function () { return null; }, 1000)];
                            case 1:
                                _a.sent();
                                newCount = cp.execSync('ps -ax | grep node | wc -l');
                                assert.strictEqual(count.toString(), newCount.toString());
                                done();
                                _a.label = 2;
                            case 2: return [2 /*return*/];
                        }
                    });
                }); });
            });
        });
    });
}
//# sourceMappingURL=unixTerminal.test.js.map