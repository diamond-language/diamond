# Language server

`lsp/` is a Language Server Protocol implementation for Diamond — a
standalone binary (`build/diamond-lsp`, built from `lsp/*.c` plus the
same compiler/VM sources every other Diamond binary shares) that speaks
LSP's standard `Content-Length`-framed JSON-RPC over stdio, the transport
every mainstream editor already knows how to launch a custom language
server with.

This is not derived from, or dependent on, any other language's LSP
implementation. Ruby's own language servers came up once in conversation
as a reference point (Diamond's syntax is Ruby-like enough that a future
syntax-highlighting grammar could plausibly start from Ruby's), but
`lsp/` itself — the JSON parser, the RPC framing, the protocol handling —
is written from scratch in C, with zero external dependencies beyond what
the rest of this repository already uses.

## What it does today

Diagnostics, plus hover/go-to-definition/document-symbols over a narrow,
declaration-only symbol table:

- `initialize` — advertises `textDocumentSync: Full` (1),
  `hoverProvider: true`, `definitionProvider: true`, and
  `documentSymbolProvider: true`. No `completionProvider`, since that
  isn't implemented; a compliant client won't ask for it.
- `textDocument/didOpen` / `didChange` / `didClose` — each recompiles the
  document's current full text (full sync only; there's no incremental
  edit application) and publishes a `textDocument/publishDiagnostics`
  notification. Diamond's compiler stops at its first error, so there is
  never more than one diagnostic per publish — an empty array means the
  document currently compiles cleanly. If the one error is actually
  inside a `require`d file rather than the open document itself, a
  *second* `publishDiagnostics` notification also goes out, against
  that file's own uri (resolved the same
  `diamond_resolve_diagnostic_location` way go-to-definition resolves a
  cross-file `Location` — see below) — this document's own publish
  still correctly reports itself clean, since its own text has no
  error. Only ever triggered by *this* document's own didOpen/didChange,
  not by editing the dependency directly; see below.
- `textDocument/hover` (`lsp/hover.c`), `textDocument/definition`
  (`lsp/definition.c`), and `textDocument/documentSymbol`
  (`lsp/document_symbol.c`) all resolve the same two identifier kinds,
  deliberately not a real general-purpose symbol table: a top-level
  function name (`def foo`, unambiguous since a bare call always
  resolves to exactly one top-level function by that name at compile
  time — no overloading, no scoping to worry about) or a class name
  (always names exactly one class, single inheritance). `src/compiler.c`
  now records each one's declaration-site position (1-based line/column
  of its own name token, plus the matching byte offset in the compiled
  buffer — `declaration_line`/`declaration_column`/`declaration_start`
  on `DiamondFunction`/`DiamondClass`, `src/vm.h`) purely for these three
  handlers to read; nothing else in the VM uses them.
  - Hover shows the reconstructed signature (parameter types, return
    type, which parameters are optional) or `class Name`/`class Name <
    Superclass`, reusing `disassemble.c`'s own type-set formatting
    (`diamond_print_type_set`).
  - Go-to-definition returns a `Location`. A match can legitimately live
    in a *different* file (something pulled in via `require`) — resolved
    to the right file and line through `diamond_resolve_diagnostic_
    location`'s segment table (`src/compiler.h`), the same machinery a
    compile error's own position already goes through, since a
    declaration's raw in-buffer line is only ever correct for the first
    thing after the most recent `#line 1` reset (`diamond_load_program`
    inserts one before *every* contiguous chunk it copies into the
    bundle, not just before required-file content) and needs the same
    segment-relative remap anywhere else.
  - Document symbols lists every top-level function/class declared *in
    that document itself* — not lib/core.di's prelude, and not anything
    pulled in through `require` (each of those has its own outline, a
    didOpen away). `range`/`selectionRange` are identical for each
    entry (just the name token — nothing tracks a declaration's full
    extent).
  - All three deliberately don't resolve a method name reached through
    `receiver.method(...)`: which class's method is meant depends on
    `receiver`'s runtime type, which nothing here infers. All three also
    require the *document* to currently compile cleanly — otherwise they
    return `null`/empty rather than a stale result; the document's own
    diagnostics already say why.
- `shutdown` / `exit` — the ordinary LSP lifecycle; `exit`'s process exit
  code is 0 if `shutdown` was requested first, 1 otherwise, per spec.
- Any other request gets a JSON-RPC `MethodNotFound` (-32601) error;
  any other notification is silently ignored.

A document's text is compiled exactly the way `src/main.c`'s own
`run_source` compiles a file natively: `require`d files resolved and
bundled in via `diamond_load_program_with_override` (`src/loader.h`),
then `lib/core.di` prepended and a `#line 1` reset so the prelude's own
line numbers never leak into a reported diagnostic's position —
confirmed against `src/lexer.c`'s handling of that exact comment, not
assumed. A diagnostic's line/column are re-resolved back through
`diamond_load_program`'s own segment table
(`diamond_resolve_diagnostic_location`, shared with `src/main.c`'s CLI
diagnostic printing — not a separate reimplementation) so they land on
the right line even when a `require`d file's inlined content shifts
everything after it.

A `require` resolving to a file that's *also* currently open sees that
document's live buffer, not stale on-disk content — even before it's
saved. `diamond_load_program_with_override`'s optional override
callback (`DiamondSourceOverride`, `src/loader.h`) is called with every
required file's own canonicalized path before falling back to disk;
`lsp/`'s every entry point that resolves `require` (diagnostics,
hover, go-to-definition, document symbols) passes
`document_resolve_source` (`lsp/document.h`), which looks that path up
against the open-document table and hands back a copy of its current
buffer if found. `diamond_load_program` itself (used by the CLI, the
REPL, `ProgramBuilder`'s native bridge) is an unchanged thin wrapper
passing no override — this is opt-in, `lsp/`-only behavior, not a
change to how requires resolve natively.

Editing an open dependency also correctly re-diagnoses whatever else
requires it, live. `lsp/dependencies.h`'s `DependencyTable` is a
reverse index — for every open document, which on-disk paths its last
compiled bundle actually pulled in (`diagnostics_compute`'s own
`out_dependency_paths`, built from the bundle's segment table, so
chains resolve in one lookup: if X requires A requires B, X's own
recorded dependency set already includes B directly, since
`diamond_load_program`'s bundle segments are fully transitive).
`lsp/main.c`'s `publish_diagnostics` looks up who currently depends on
a document right after publishing its own diagnostics, and republishes
theirs too — each of those requires now resolving through the
just-changed document's live buffer via the override above, so a
dependent's diagnostics genuinely reflect the edit, not a stale
snapshot from before it.

## Building and connecting an editor

```sh
make lsp
```

produces `build/diamond-lsp`. Point any editor's "custom language server"
configuration at that binary with stdio as the transport (no `--stdio`
flag needed or accepted — stdio is the only transport this server
speaks). The exact configuration step is editor-specific; consult your
editor's LSP client documentation for how it registers a server for a
new file type/extension (`.di`).

`make test-lsp` runs `tests/lsp_test.sh`, which drives the real binary
over its real stdio transport via a bash `coproc` — the same
no-Python/no-Node convention every other test script in this repo
follows — through the full `initialize` → `didOpen` → `didChange` →
`didClose` → `shutdown` → `exit` lifecycle, hover/definition/
documentSymbol at a declaration, at a call site, and across a `require`,
all three correctly returning nothing for a local variable or a document
that doesn't currently compile, document symbols excluding the prelude
and anything pulled in via `require`, plus the unknown-method and
exit-without-shutdown edge cases.

`editors/vscode/extension.js` wires all three up client-side too, via
`vscode.languages.registerHoverProvider`/`registerDefinitionProvider`/
`registerDocumentSymbolProvider` — see its own file and
`editors/vscode/README.md`.

## What's deliberately out of scope so far

- **Completion, workspace-wide symbol search** — need a real, general
  symbol table (name → declaration site, with actual scope resolution),
  which nothing in `lsp/` builds. Hover/definition/documentSymbol get by
  without one by resolving only two globally-unambiguous identifier
  kinds (see above); completion in particular can't take that shortcut
  at all — "what names are valid here" is a different question than
  "where is this one declared" and needs real scope resolution to
  answer honestly.
- **Hover/definition/documentSymbol on a method name reached through
  `receiver.method(...)`** — needs type inference on `receiver` to know
  which class's method is meant (possibly several classes define a
  same-named method); see above.
- **Incremental sync** — `textDocumentSync` only ever advertises `Full`.
  Diamond has no incremental-recompile story at all yet (every compile is
  a fresh `diamond_compile` call over the whole combined buffer), so
  `Incremental` sync would only add bookkeeping for no benefit until that
  changes.
- **Any transport but stdio** — no TCP/socket mode. Every mainstream
  editor already launches custom language servers over stdio, so there's
  no immediate need for another one.

Each of these is a plausible next slice, sized independently — see
`docs/roadmap.md`.
