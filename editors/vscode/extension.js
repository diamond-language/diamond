'use strict';

/* A hand-rolled LSP client, not `vscode-languageclient` -- matching
 * `lsp/`'s own "written from scratch, zero external dependencies"
 * convention (see docs/lsp.md) on the client side too. `diamond-lsp`
 * only ever speaks a small, fixed subset of LSP (see below), so the
 * generality a full client library buys isn't needed, and this stays
 * plain CommonJS requiring nothing but Node/Electron builtins -- no
 * node_modules, no npm install, no build step, same promise this
 * extension's syntax-highlighting half already makes.
 *
 * Protocol surface this client relies on, all confirmed against
 * `lsp/main.c`/`lsp/rpc.c` directly rather than assumed from the LSP
 * spec in general: `Content-Length: N\r\n\r\n<json>` framing with no
 * other headers; `initialize` ignores its params entirely and always
 * replies with `{capabilities: {textDocumentSync: 1}}` (Full sync, so
 * every didChange below sends the whole document, never a range edit);
 * `shutdown` replies with a null result; `exit` gets no reply and ends
 * the process; unknown methods get a JSON-RPC MethodNotFound error only
 * if they were a request (had an id) -- notifications are silently
 * dropped, so sending `initialized` is harmless even though the server
 * never reads it. */

const vscode = require('vscode');
const cp = require('child_process');

let child = null;
let stdoutBuffer = Buffer.alloc(0);
let nextRequestId = 1;
const pendingRequests = new Map();
let diagnosticCollection = null;
let outputChannel = null;

function log(line) {
    if (outputChannel) outputChannel.appendLine(line);
}

function severityToVscode(severity) {
    switch (severity) {
        case 2: return vscode.DiagnosticSeverity.Warning;
        case 3: return vscode.DiagnosticSeverity.Information;
        case 4: return vscode.DiagnosticSeverity.Hint;
        default: return vscode.DiagnosticSeverity.Error;
    }
}

function writeMessage(message) {
    if (!child || !child.stdin.writable) return;
    const body = Buffer.from(JSON.stringify(message), 'utf8');
    const header = Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, 'ascii');
    try {
        child.stdin.write(Buffer.concat([header, body]));
    } catch (err) {
        log(`write failed: ${err.message}`);
    }
}

function sendRequest(method, params) {
    if (!child) return Promise.reject(new Error('language server is not running'));
    const id = nextRequestId++;
    return new Promise((resolve, reject) => {
        pendingRequests.set(id, { resolve, reject });
        writeMessage({ jsonrpc: '2.0', id, method, params });
    });
}

function sendNotification(method, params) {
    writeMessage({ jsonrpc: '2.0', method, params });
}

/* Consumes as many complete `Content-Length`-framed messages as are
 * currently buffered, leaving any trailing partial message for the next
 * chunk -- `stdout` delivers arbitrary byte chunks, not one message per
 * event. */
function onStdout(chunk) {
    stdoutBuffer = Buffer.concat([stdoutBuffer, chunk]);
    while (true) {
        const headerEnd = stdoutBuffer.indexOf('\r\n\r\n');
        if (headerEnd === -1) return;
        const header = stdoutBuffer.slice(0, headerEnd).toString('ascii');
        const match = /Content-Length:\s*(\d+)/i.exec(header);
        if (!match) {
            log(`malformed header, dropping: ${header}`);
            stdoutBuffer = stdoutBuffer.slice(headerEnd + 4);
            continue;
        }
        const length = parseInt(match[1], 10);
        const bodyStart = headerEnd + 4;
        if (stdoutBuffer.length < bodyStart + length) return;
        const body = stdoutBuffer.slice(bodyStart, bodyStart + length).toString('utf8');
        stdoutBuffer = stdoutBuffer.slice(bodyStart + length);
        let message;
        try {
            message = JSON.parse(body);
        } catch (err) {
            log(`failed to parse message body: ${err.message}`);
            continue;
        }
        handleMessage(message);
    }
}

function handleMessage(message) {
    if (typeof message.method === 'string') {
        if (message.method === 'textDocument/publishDiagnostics') {
            handlePublishDiagnostics(message.params);
        }
        return;
    }
    if (message.id === undefined || message.id === null) return;
    const pending = pendingRequests.get(message.id);
    if (!pending) return;
    pendingRequests.delete(message.id);
    if (message.error) pending.reject(new Error(message.error.message || 'language server error'));
    else pending.resolve(message.result);
}

function handlePublishDiagnostics(params) {
    if (!params || typeof params.uri !== 'string' || !Array.isArray(params.diagnostics)) return;
    let uri;
    try {
        uri = vscode.Uri.parse(params.uri);
    } catch (err) {
        return;
    }
    const diagnostics = params.diagnostics.map((entry) => {
        const range = new vscode.Range(
            entry.range.start.line, entry.range.start.character,
            entry.range.end.line, entry.range.end.character,
        );
        const diagnostic = new vscode.Diagnostic(range, entry.message, severityToVscode(entry.severity));
        diagnostic.source = entry.source || 'diamond';
        return diagnostic;
    });
    diagnosticCollection.set(uri, diagnostics);
}

function didOpen(document) {
    if (document.languageId !== 'diamond' || !child) return;
    sendNotification('textDocument/didOpen', {
        textDocument: {
            uri: document.uri.toString(),
            languageId: 'diamond',
            version: document.version,
            text: document.getText(),
        },
    });
}

function didChange(document) {
    if (document.languageId !== 'diamond' || !child) return;
    /* Server only advertises Full sync -- send the whole current buffer
     * as the one entry Full sync expects, ignoring whatever incremental
     * ranges VS Code's own event carried. */
    sendNotification('textDocument/didChange', {
        textDocument: { uri: document.uri.toString(), version: document.version },
        contentChanges: [{ text: document.getText() }],
    });
}

function didClose(document) {
    if (document.languageId !== 'diamond' || !child) return;
    /* No client-side diagnosticCollection.delete() here: the server
     * itself publishes an empty diagnostics array for this uri on
     * didClose (its own documented way to clear a closed file's
     * diagnostics), which arrives back through handlePublishDiagnostics
     * asynchronously. Clearing here too would race it -- this delete
     * could win before that publish lands, and the server's own publish
     * would then put an (empty, harmless, but stale-looking) entry right
     * back. */
    sendNotification('textDocument/didClose', {
        textDocument: { uri: document.uri.toString() },
    });
}

function startServer() {
    const serverPath = vscode.workspace.getConfiguration('diamond').get('languageServerPath', 'diamond-lsp');
    let spawned;
    try {
        spawned = cp.spawn(serverPath, [], { stdio: ['pipe', 'pipe', 'pipe'] });
    } catch (err) {
        vscode.window.showErrorMessage(`Diamond: failed to start '${serverPath}': ${err.message}`);
        return;
    }
    child = spawned;
    stdoutBuffer = Buffer.alloc(0);
    spawned.stdout.on('data', onStdout);
    spawned.stderr.on('data', (data) => log(`[diamond-lsp stderr] ${data.toString('utf8')}`));
    /* A spawn failure (e.g. ENOENT for a missing/misconfigured path)
     * surfaces asynchronously as an 'error' event, both on the child
     * itself below and, since stdin is already a live stream by the
     * time that happens, on stdin too -- an unhandled 'error' event on
     * any EventEmitter is a thrown exception in Node, so this needs its
     * own listener even though the child's own handler already shows
     * the user a message. */
    spawned.stdin.on('error', (err) => log(`stdin error: ${err.message}`));
    /* A launch failure (e.g. ENOENT for a missing/misconfigured path)
     * only ever emits 'error' on at least some platforms/Node versions
     * -- not a following 'exit' -- so `child` has to be cleared here
     * too, not just in the 'exit' handler below. Leaving it set would
     * make every later stopServer() (deactivate, the restart command)
     * think a real process is still there, send it a 'shutdown' request
     * that nothing will ever answer, and hang forever awaiting a
     * response that can't arrive -- confirmed by testing this exact
     * path against Node's actual ENOENT behavior, not assumed from the
     * docs. */
    spawned.on('error', (err) => {
        vscode.window.showErrorMessage(
            `Diamond: language server '${serverPath}' failed to start: ${err.message}. ` +
            'Build it with `make lsp`, or set "diamond.languageServerPath" to the built binary.',
        );
        if (child === spawned) child = null;
    });
    spawned.on('exit', (code, signal) => {
        log(`diamond-lsp exited (code=${code}, signal=${signal})`);
        if (child === spawned) child = null;
    });

    sendRequest('initialize', {
        processId: process.pid,
        rootUri: vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders.length > 0
            ? vscode.workspace.workspaceFolders[0].uri.toString()
            : null,
        capabilities: {},
    }).then(() => {
        sendNotification('initialized', {});
        vscode.workspace.textDocuments.forEach(didOpen);
    }).catch((err) => {
        log(`initialize failed: ${err.message}`);
    });
}

function stopServer() {
    if (!child) return Promise.resolve();
    const proc = child;
    /* sendRequest('shutdown', ...) only ever settles when a matching
     * response arrives over stdout -- if the process is wedged (or its
     * pipes are, e.g. a launch that half-succeeded), that response never
     * comes and the request would otherwise hang forever. Racing it
     * against a timeout keeps this bounded either way; the kill-after-
     * 2s fallback below still runs regardless. */
    const shutdownReply = sendRequest('shutdown', null);
    const shutdownTimeout = new Promise((resolve) => setTimeout(resolve, 1000));
    return Promise.race([shutdownReply.catch(() => {}), shutdownTimeout])
        .then(() => sendNotification('exit', null))
        .then(() => new Promise((resolve) => {
            const timer = setTimeout(() => {
                try { proc.kill(); } catch (err) { /* already gone */ }
                resolve();
            }, 2000);
            proc.once('exit', () => {
                clearTimeout(timer);
                resolve();
            });
        }));
}

function activate(context) {
    outputChannel = vscode.window.createOutputChannel('Diamond Language Server');
    diagnosticCollection = vscode.languages.createDiagnosticCollection('diamond');
    context.subscriptions.push(outputChannel, diagnosticCollection);

    context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(didOpen));
    context.subscriptions.push(vscode.workspace.onDidChangeTextDocument((event) => didChange(event.document)));
    context.subscriptions.push(vscode.workspace.onDidCloseTextDocument(didClose));
    context.subscriptions.push(vscode.commands.registerCommand('diamond.restartLanguageServer', async () => {
        await stopServer();
        startServer();
    }));

    startServer();
}

function deactivate() {
    return stopServer();
}

module.exports = { activate, deactivate };
