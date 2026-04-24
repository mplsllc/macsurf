"use strict";
/**
 * Copyright (c) 2017, Daniel Imms (MIT License).
 * Copyright (c) 2018, Microsoft Corporation (MIT License).
 */
Object.defineProperty(exports, "__esModule", { value: true });
var fs = require("fs");
var assert = require("assert");
var windowsTerminal_1 = require("./windowsTerminal");
var path = require("path");
var psList = require("ps-list");
function pollForProcessState(desiredState, intervalMs, timeoutMs) {
    if (intervalMs === void 0) { intervalMs = 100; }
    if (timeoutMs === void 0) { timeoutMs = 2000; }
    return new Promise(function (resolve) {
        var tries = 0;
        var interval = setInterval(function () {
            psList({ all: true }).then(function (ps) {
                var success = true;
                var pids = Object.keys(desiredState).map(function (k) { return parseInt(k, 10); });
                console.log('expected pids', JSON.stringify(pids));
                pids.forEach(function (pid) {
                    if (desiredState[pid]) {
                        if (!ps.some(function (p) { return p.pid === pid; })) {
                            console.log("pid " + pid + " does not exist");
                            success = false;
                        }
                    }
                    else {
                        if (ps.some(function (p) { return p.pid === pid; })) {
                            console.log("pid " + pid + " still exists");
                            success = false;
                        }
                    }
                });
                if (success) {
                    clearInterval(interval);
                    resolve();
                    return;
                }
                tries++;
                if (tries * intervalMs >= timeoutMs) {
                    clearInterval(interval);
                    var processListing = pids.map(function (k) { return k + ": " + desiredState[k]; }).join('\n');
                    assert.fail("Bad process state, expected:\n" + processListing);
                    resolve();
                }
            });
        }, intervalMs);
    });
}
function pollForProcessTreeSize(pid, size, intervalMs, timeoutMs) {
    if (intervalMs === void 0) { intervalMs = 100; }
    if (timeoutMs === void 0) { timeoutMs = 2000; }
    return new Promise(function (resolve) {
        var tries = 0;
        var interval = setInterval(function () {
            psList({ all: true }).then(function (ps) {
                var openList = [];
                openList.push(ps.filter(function (p) { return p.pid === pid; }).map(function (p) {
                    return { name: p.name, pid: p.pid };
                })[0]);
                var list = [];
                var _loop_1 = function () {
                    var current = openList.shift();
                    ps.filter(function (p) { return p.ppid === current.pid; }).map(function (p) {
                        return { name: p.name, pid: p.pid };
                    }).forEach(function (p) { return openList.push(p); });
                    list.push(current);
                };
                while (openList.length) {
                    _loop_1();
                }
                console.log('list', JSON.stringify(list));
                var success = list.length === size;
                if (success) {
                    clearInterval(interval);
                    resolve(list);
                    return;
                }
                tries++;
                if (tries * intervalMs >= timeoutMs) {
                    clearInterval(interval);
                    assert.fail("Bad process state, expected: " + size + ", actual: " + list.length);
                }
            });
        }, intervalMs);
    });
}
if (process.platform === 'win32') {
    [[false, false], [true, false], [true, true]].forEach(function (_a) {
        var useConpty = _a[0], useConptyDll = _a[1];
        describe("WindowsTerminal (useConpty = " + useConpty + ", useConptyDll = " + useConptyDll + ")", function () {
            describe('kill', function () {
                it('should not crash parent process', function (done) {
                    this.timeout(20000);
                    var term = new windowsTerminal_1.WindowsTerminal('cmd.exe', [], { useConpty: useConpty, useConptyDll: useConptyDll });
                    term.on('exit', function () { return done(); });
                    term.kill();
                });
                it('should kill the process tree', function (done) {
                    this.timeout(20000);
                    var term = new windowsTerminal_1.WindowsTerminal('cmd.exe', [], { useConpty: useConpty, useConptyDll: useConptyDll });
                    // Start sub-processes
                    term.write('powershell.exe\r');
                    term.write('node.exe\r');
                    console.log('start poll for tree size');
                    pollForProcessTreeSize(term.pid, 3, 500, 5000).then(function (list) {
                        assert.strictEqual(list[0].name.toLowerCase(), 'cmd.exe');
                        assert.strictEqual(list[1].name.toLowerCase(), 'powershell.exe');
                        assert.strictEqual(list[2].name.toLowerCase(), 'node.exe');
                        term.kill();
                        var desiredState = {};
                        desiredState[list[0].pid] = false;
                        desiredState[list[1].pid] = false;
                        desiredState[list[2].pid] = false;
                        term.on('exit', function () {
                            pollForProcessState(desiredState, 1000, 5000).then(function () {
                                done();
                            });
                        });
                    });
                });
            });
            describe('resize', function () {
                it('should throw a non-native exception when resizing an invalid value', function (done) {
                    this.timeout(20000);
                    var term = new windowsTerminal_1.WindowsTerminal('cmd.exe', [], { useConpty: useConpty, useConptyDll: useConptyDll });
                    assert.throws(function () { return term.resize(-1, -1); });
                    assert.throws(function () { return term.resize(0, 0); });
                    assert.doesNotThrow(function () { return term.resize(1, 1); });
                    term.on('exit', function () {
                        done();
                    });
                    term.kill();
                });
                it('should throw a non-native exception when resizing a killed terminal', function (done) {
                    this.timeout(20000);
                    var term = new windowsTerminal_1.WindowsTerminal('cmd.exe', [], { useConpty: useConpty, useConptyDll: useConptyDll });
                    term._defer(function () {
                        term.once('exit', function () {
                            assert.throws(function () { return term.resize(1, 1); });
                            done();
                        });
                        term.destroy();
                    });
                });
            });
            describe('Args as CommandLine', function () {
                it('should not fail running a file containing a space in the path', function (done) {
                    this.timeout(10000);
                    var spaceFolder = path.resolve(__dirname, '..', 'fixtures', 'space folder');
                    if (!fs.existsSync(spaceFolder)) {
                        fs.mkdirSync(spaceFolder);
                    }
                    var cmdCopiedPath = path.resolve(spaceFolder, 'cmd.exe');
                    var data = fs.readFileSync(process.env.windir + "\\System32\\cmd.exe");
                    fs.writeFileSync(cmdCopiedPath, data);
                    if (!fs.existsSync(cmdCopiedPath)) {
                        // Skip test if git bash isn't installed
                        return;
                    }
                    var term = new windowsTerminal_1.WindowsTerminal(cmdCopiedPath, '/c echo "hello world"', { useConpty: useConpty, useConptyDll: useConptyDll });
                    var result = '';
                    term.on('data', function (data) {
                        result += data;
                    });
                    term.on('exit', function () {
                        assert.ok(result.indexOf('hello world') >= 1);
                        done();
                    });
                });
            });
            describe('env', function () {
                it('should set environment variables of the shell', function (done) {
                    this.timeout(10000);
                    var term = new windowsTerminal_1.WindowsTerminal('cmd.exe', '/C echo %FOO%', { useConpty: useConpty, useConptyDll: useConptyDll, env: { FOO: 'BAR' } });
                    var result = '';
                    term.on('data', function (data) {
                        result += data;
                    });
                    term.on('exit', function () {
                        assert.ok(result.indexOf('BAR') >= 0);
                        done();
                    });
                });
            });
            describe('On close', function () {
                it('should return process zero exit codes', function (done) {
                    this.timeout(10000);
                    var term = new windowsTerminal_1.WindowsTerminal('cmd.exe', '/C exit', { useConpty: useConpty, useConptyDll: useConptyDll });
                    term.on('exit', function (code) {
                        assert.strictEqual(code, 0);
                        done();
                    });
                });
                it('should return process non-zero exit codes', function (done) {
                    this.timeout(10000);
                    var term = new windowsTerminal_1.WindowsTerminal('cmd.exe', '/C exit 2', { useConpty: useConpty, useConptyDll: useConptyDll });
                    term.on('exit', function (code) {
                        assert.strictEqual(code, 2);
                        done();
                    });
                });
            });
            describe('Write', function () {
                it('should accept input', function (done) {
                    this.timeout(10000);
                    var term = new windowsTerminal_1.WindowsTerminal('cmd.exe', '', { useConpty: useConpty, useConptyDll: useConptyDll });
                    term.write('exit\r');
                    term.on('exit', function () {
                        done();
                    });
                });
            });
        });
    });
}
//# sourceMappingURL=windowsTerminal.test.js.map