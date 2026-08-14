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
  document currently compiles cleanly.
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
bundled in via `diamond_load_program` (read from disk — so a `require`
only resolves correctly once the document has a real `file://` uri
*and* its dependencies exist on disk at their expected relative
locations; an unsaved dependency's in-editor-only edits aren't seen),
then `lib/core.di` prepended and a `#line 1` reset so the prelude's own
line numbers never leak into a reported diagnostic's position —
confirmed against `src/lexer.c`'s handling of that exact comment, not
assumed. A diagnostic's line/column are re-resolved back through
`diamond_load_program`'s own segment table
(`diamond_resolve_diagnostic_location`, shared with `src/main.c`'s CLI
diagnostic printing — not a separate reimplementation) so they land on
the right line even when a `require`d file's inlined content shifts
everything after it. One consequence worth knowing: if the *actual*
error is inside a `require`d file rather than the open document itself,
this document currently reports nothing at all (not the dependency's
error misattributed to the wrong file, but not the dependency's own
diagnostic either) — publishing a second `publishDiagnostics`
notification against the dependency's own uri is real, separable work
for later.

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
- **Diagnostics for a broken dependency, published against its own
  file** — `require` itself resolves (see above), but if the error is
  inside the required file rather than the open document, nothing gets
  published for it at all yet. Doing this properly means tracking which
  open documents depend on which files (so editing a dependency
  re-diagnoses everything that requires it, not just itself) — real,
  separable work.
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
