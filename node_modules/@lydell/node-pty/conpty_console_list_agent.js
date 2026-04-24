"use strict";
/**
 * Copyright (c) 2019, Microsoft Corporation (MIT License).
 *
 * This module fetches the console process list for a particular PID. It must be
 * called from a different process (child_process.fork) as there can only be a
 * single console attached to a process.
 */
var getConsoleProcessList = require("@lydell/node-pty-" + process.platform + "-" + process.arch + "/conpty_console_list.node").getConsoleProcessList;
var shellPid = parseInt(process.argv[2], 10);
var consoleProcessList = getConsoleProcessList(shellPid);
process.send({ consoleProcessList: consoleProcessList });
process.exit(0);
