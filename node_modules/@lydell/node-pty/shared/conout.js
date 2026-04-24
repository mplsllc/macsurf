"use strict";
/**
 * Copyright (c) 2020, Microsoft Corporation (MIT License).
 */
Object.defineProperty(exports, "__esModule", { value: true });
function getWorkerPipeName(conoutPipeName) {
    return conoutPipeName + "-worker";
}
exports.getWorkerPipeName = getWorkerPipeName;
