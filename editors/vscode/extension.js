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
 * other headers; `initialize`'s own params only ever matter for
 * `rootUri` (read once, to resolve workspace/symbol's search root --
 * see docs/lsp.md), always replies with `{capabilities: {...}}`
 * advertising Full sync (so every didChange below sends the whole
 * document, never a range edit) plus whichever of hover/definition/
 * documentSymbol/completion/workspaceSymbol `lsp/main.c`'s own
 * `handle_initialize` currently supports; `shutdown` replies with a
 * null result; `exit` gets no reply and ends the process; unknown
 * methods get a JSON-RPC MethodNotFound error only if they were a
 * request (had an id) -- notifications are silently dropped, so
 * sending `initialized` is harmless even though the server never reads
 * it. */

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

/* 'diamond' (.di) and 'diamond-template' (.div, packages/div -- see
 * lsp/div.c) both get diagnostics; only 'diamond' additionally gets
 * hover/definition/documentSymbol/completion, via those providers'
 * own separate 'diamond'-only registrations below (docs/lsp.md's own
 * ".div templates (diagnostics only)" section). */
function isDiamondDocument(document) {
    return document.languageId === 'diamond' || document.languageId === 'diamond-template';
}

function didOpen(document) {
    if (!isDiamondDocument(document) || !child) return;
    sendNotification('textDocument/didOpen', {
        textDocument: {
            uri: document.uri.toString(),
            languageId: document.languageId,
            version: document.version,
            text: document.getText(),
        },
    });
}

function didChange(document) {
    if (!isDiamondDocument(document) || !child) return;
    /* Server only advertises Full sync -- send the whole current buffer
     * as the one entry Full sync expects, ignoring whatever incremental
     * ranges VS Code's own event carried. */
    sendNotification('textDocument/didChange', {
        textDocument: { uri: document.uri.toString(), version: document.version },
        contentChanges: [{ text: document.getText() }],
    });
}

function didClose(document) {
    if (!isDiamondDocument(document) || !child) return;
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

/* vscode.HoverProvider#provideHover. Every open .di document already
 * has a live server-side copy from didOpen/didChange, so this only ever
 * needs to ask about it -- no separate "send the text along with the
 * hover request" step the way an unopened document might need. */
async function provideHover(document, position) {
    if (!child) return undefined;
    let raw;
    try {
        raw = await sendRequest('textDocument/hover', {
            textDocument: { uri: document.uri.toString() },
            position: { line: position.line, character: position.character },
        });
    } catch (err) {
        return undefined;
    }
    if (!raw || !raw.contents || typeof raw.contents.value !== 'string') return undefined;
    return new vscode.Hover(raw.contents.value);
}

/* vscode.DefinitionProvider#provideDefinition. The server's Location
 * may name a *different* file than the one being edited (a symbol
 * pulled in through `require`) -- vscode.Uri.parse handles that uri
 * exactly the same way regardless of which file it names, no special
 * casing needed here. */
async function provideDefinition(document, position) {
    if (!child) return undefined;
    let raw;
    try {
        raw = await sendRequest('textDocument/definition', {
            textDocument: { uri: document.uri.toString() },
            position: { line: position.line, character: position.character },
        });
    } catch (err) {
        return undefined;
    }
    if (!raw || !raw.uri || !raw.range) return undefined;
    return new vscode.Location(vscode.Uri.parse(raw.uri), toRange(raw.range));
}

function toRange(raw) {
    return new vscode.Range(
        raw.start.line, raw.start.character,
        raw.end.line, raw.end.character,
    );
}

/* vscode.DocumentSymbolProvider#provideDocumentSymbols. vscode.SymbolKind
 * is 0-indexed (File=0) while the LSP wire protocol's SymbolKind is
 * 1-indexed (File=1, matching the spec) -- every entry.kind sent over
 * the wire needs that -1 applied, not just the two kinds this server
 * happens to emit today (Function=12, Class=5). */
async function provideDocumentSymbols(document) {
    if (!child) return undefined;
    let raw;
    try {
        raw = await sendRequest('textDocument/documentSymbol', {
            textDocument: { uri: document.uri.toString() },
        });
    } catch (err) {
        return undefined;
    }
    if (!Array.isArray(raw)) return undefined;
    return raw.map((entry) => new vscode.DocumentSymbol(
        entry.name, '', entry.kind - 1, toRange(entry.range), toRange(entry.selectionRange),
    ));
}

/* vscode.CompletionItemProvider#provideCompletionItems. vscode.
 * CompletionItemKind is 0-indexed (Text=0) while the LSP wire
 * protocol's CompletionItemKind is 1-indexed (Text=1, matching the
 * spec) -- same -1 shift provideDocumentSymbols/provideWorkspaceSymbols
 * already need for their own (differently-numbered) SymbolKind. The
 * server returns the same full candidate list regardless of what's
 * already typed (see lsp/completion.h) and relies on vscode's own
 * built-in prefix narrowing to filter it live -- no triggerCharacters
 * needed, matching the empty CompletionOptions the server's own
 * capabilities advertise. */
async function provideCompletionItems(document, position) {
    if (!child) return undefined;
    let raw;
    try {
        raw = await sendRequest('textDocument/completion', {
            textDocument: { uri: document.uri.toString() },
            position: { line: position.line, character: position.character },
        });
    } catch (err) {
        return undefined;
    }
    if (!Array.isArray(raw)) return undefined;
    return raw.map((entry) => new vscode.CompletionItem(entry.name || entry.label, entry.kind - 1));
}

/* vscode.WorkspaceSymbolProvider#provideWorkspaceSymbols. Unlike every
 * other provider here, the server resolves the workspace root itself
 * from `initialize`'s own rootUri (sent once, below) rather than this
 * request's params -- there's no per-request "which folder" to pass
 * along the way there is for VS Code's own multi-root workspaces, so a
 * multi-root workspace only ever searches its first folder; a real gap
 * if that ever matters, not one this project has hit yet. */
async function provideWorkspaceSymbols(query) {
    if (!child) return undefined;
    let raw;
    try {
        raw = await sendRequest('workspace/symbol', { query });
    } catch (err) {
        return undefined;
    }
    if (!Array.isArray(raw)) return undefined;
    return raw.map((entry) => new vscode.SymbolInformation(
        entry.name, entry.kind - 1, '',
        new vscode.Location(vscode.Uri.parse(entry.location.uri), toRange(entry.location.range)),
    ));
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

/* diamond-dap already speaks real DAP over its own stdio (Content-
 * Length-framed JSON, see dap/main.c) -- unlike the hand-rolled LSP
 * client above, nothing here needs to parse that protocol itself: VS
 * Code's own `vscode.debug` machinery does, the moment it's told which
 * executable to spawn. Same `diamond.<name>Path`-configuration-setting
 * convention as `languageServerPath` (startServer above), for the same
 * reason: a built-but-not-installed binary, or one under a name PATH
 * doesn't already resolve. */
function registerDebugAdapter(context) {
    context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory('diamond', {
        createDebugAdapterDescriptor() {
            const adapterPath = vscode.workspace.getConfiguration('diamond')
                .get('debugAdapterPath', 'diamond-dap');
            return new vscode.DebugAdapterExecutable(adapterPath, []);
        },
    }));
}

function activate(context) {
    outputChannel = vscode.window.createOutputChannel('Diamond Language Server');
    diagnosticCollection = vscode.languages.createDiagnosticCollection('diamond');
    context.subscriptions.push(outputChannel, diagnosticCollection);

    context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(didOpen));
    context.subscriptions.push(vscode.workspace.onDidChangeTextDocument((event) => didChange(event.document)));
    context.subscriptions.push(vscode.workspace.onDidCloseTextDocument(didClose));
    context.subscriptions.push(vscode.languages.registerHoverProvider('diamond', { provideHover }));
    context.subscriptions.push(vscode.languages.registerDefinitionProvider('diamond', { provideDefinition }));
    context.subscriptions.push(vscode.languages.registerDocumentSymbolProvider('diamond', { provideDocumentSymbols }));
    context.subscriptions.push(vscode.languages.registerCompletionItemProvider('diamond', { provideCompletionItems }));
    context.subscriptions.push(vscode.languages.registerWorkspaceSymbolProvider({ provideWorkspaceSymbols }));
    context.subscriptions.push(vscode.commands.registerCommand('diamond.restartLanguageServer', async () => {
        await stopServer();
        startServer();
    }));
    registerDebugAdapter(context);

    startServer();
}

function deactivate() {
    return stopServer();
}

module.exports = { activate, deactivate };
