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

Diagnostics, hover/go-to-definition/document-symbols over a narrow,
declaration-only symbol table, plus completion and workspace-wide symbol
search over a real (if scoped) lexical symbol table:

- `initialize` — advertises `textDocumentSync: Full` (1),
  `hoverProvider: true`, `definitionProvider: true`,
  `documentSymbolProvider: true`, `completionProvider: {}` (no
  `triggerCharacters` — see completion below for why none are needed),
  and `workspaceSymbolProvider: true`. Also reads `workspaceFolders[0]`/
  `rootUri` from the request's own params (the one place this server
  reads anything from `initialize`'s params at all) to know what
  directory `workspace/symbol` should search.
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
  (`lsp/document_symbol.c`) all resolve the same declaration kinds,
  deliberately not a real general-purpose symbol table: a top-level
  function name (`def foo`, unambiguous since a bare call always
  resolves to exactly one top-level function by that name at compile
  time — no overloading, no scoping to worry about), a class name
  (always names exactly one class, single inheritance), a module name,
  or an interface name. `src/compiler.c` records each one's declaration-site position (1-based line/column
  of its own name token, plus the matching byte offset in the compiled
  buffer — `declaration_line`/`declaration_column`/`declaration_start`
  on `DiamondFunction`/`DiamondClass`/`DiamondModule`/`DiamondInterface`,
  `src/vm.h`) purely for these three
  handlers to read; nothing else in the VM uses them.
  - Hover shows the reconstructed signature (parameter types, return
    type, which parameters are optional), `class Name`/`class Name <
    Superclass`, `module Name`, or `interface Name`, reusing
    `disassemble.c`'s own type-set formatting
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
  - Document symbols lists every top-level function, class, module, and
    interface declared *in
    that document itself* — not lib/core.di's prelude, and not anything
    pulled in through `require` (each of those has its own outline, a
    didOpen away). `range`/`selectionRange` are identical for each
    entry (just the name token — nothing tracks a declaration's full
    extent).
  - All three also fall back to `lsp/receiver.c` for a method name
    reached through `receiver.method(...)`, for the receiver forms
    resolvable without real type inference: a literal class name
    (`Author.find`), `self` inside an instance method or a class-owned
    `def self.x` (wall 2's `DIAMOND_VALUE_CLASS`, `docs/design.md`), a
    local variable last known (at its own declaration) to hold
    `ClassName.new(...)` — `DiamondScopeLocal.known_type` (`src/vm.h`),
    a byte snapshot of the compiler's own `known_types[reg]` at the
    exact moment `record_scope_locals` closes that local's scope, the
    only new compiler-side state this needed — or a parameter (or a
    local initialized from one) given an explicit union annotation
    (`pet: Dog | Cat`) — `DiamondScopeLocal.known_type_set` (`src/vm.h`),
    the same snapshot idea applied to the compiler's own
    `known_type_sets[reg]`, decoded against the *owning function's own*
    `type_sets[]` table (a set index means nothing against any other
    function's). `self`'s own enclosing class comes from
    `DiamondFunction.owner_class` via `DiamondFunction.body_end`
    (`src/vm.h`, populated at every function-closing site alongside
    `declaration_start` so even a zero-parameter, zero-local method — a
    very common `self.foo()` one-liner shape — still has a real body
    extent to locate `self` within); `receiver_lookup_method` then walks
    each candidate class and its superclass chain by name, mirroring
    `lookup_method`/`lookup_singleton_method` (`src/vm.c`) at the source
    level. A union receiver resolves against *every* class-kind member
    that defines the method: hover shows one signature when every match
    agrees on it textually, or `ClassName#signature` per match when they
    differ; go-to-definition returns a `Location[]` when there's more
    than one match (single `Location` otherwise, unchanged); completion
    lists every candidate's own methods, undeduplicated. **A union type
    only ever comes from an explicit source-level annotation** — the
    compiler does not build one from a branching assignment
    (`x = cond ? Dog.new() : Cat.new()` leaves `x` with no tracked type
    at all, not a two-member union; `parse_if`'s merge logic only keeps
    a type-set match when both branches already agree on the exact same
    set index, `src/compiler.c`), so that shape still falls back to
    "not found" here, same as before this slice. An instance-variable
    receiver, a chained call (`foo().bar()`), a local reassigned to a
    different class after its own declaration, or any other receiver
    this can't resolve also falls back to the ordinary "not found"
    result instead of a guess. All three also require the *document* to
    currently compile cleanly — otherwise they return `null`/empty
    rather than a stale result; the document's own diagnostics already
    say why.
- `textDocument/completion` (`lsp/completion.c`) suggests every
  top-level function, class, module, and interface in the whole compiled program (not just this
  document's own — `lib/core.di`'s prelude and anything pulled in via
  `require` are all valid to type) plus every local variable/parameter
  actually *in scope at the cursor* — real lexical scoping, backed by a
  genuine (if narrow) per-function symbol table `src/compiler.c` now
  builds as a side effect of compiling: `DiamondFunction.scope_locals`
  (`src/vm.h`) records every local's own name and the exact byte range
  (in the compiled buffer) it's valid for, taken directly from the
  compiler's own `Local` bookkeeping at the two points a scope actually
  closes (a function body finishing in `compile_definition`, a `rescue`
  clause finishing in `compile_begin` — Diamond's `if`/`while`/`unless`/
  `until` deliberately *don't* open their own scope, matching Ruby, so
  those needed no extra handling). Mapping the cursor's own line/column
  into that same compiled-buffer coordinate system needed a new inverse
  of `diamond_resolve_diagnostic_location`: `diamond_resolve_source_
  position` (`src/compiler.h`) walks the same segment table the other
  direction. A name's valid range is checked directly against the
  cursor's own offset — no lexical-nesting bookkeeping needed for a
  nested `def` to see its enclosing function's own locals (real closure
  capture visibility), since the outer function's own recorded range
  already spans everything nested inside it, nested `def`s included.
  Doesn't filter by whatever's already typed (every mainstream client
  already does that client-side against the full list this returns) or
  suggest language keywords; requires a clean compile, same rule as
  hover/definition/documentSymbol. When the cursor sits right after
  `receiver.` (method name partially typed or not yet typed at all),
  also appends the receiver's own class's methods and its superclass
  chain's, via the same `lsp/receiver.c` resolution hover/definition
  use — not deduplicated against each other or the rest of the list, same
  as nothing else here dedupes either.
- `workspace/symbol` (`lsp/workspace_symbol.c`) recursively walks the
  workspace root given via `initialize` (skipping dotfiles/dotdirs —
  `.git` and friends), compiles every `*.di` file it finds the same way
  an open document is (an open file's own live, possibly-unsaved buffer
  is preferred over disk, same as everywhere else `require` resolves),
  and returns every top-level function, class, module, and interface actually declared *in that
  file itself* whose name contains the query as a case-insensitive
  substring. No caching across separate requests — real editors only
  send this on an explicit "go to symbol in workspace" action, not on
  every keystroke, so re-walking and re-compiling the whole workspace
  each time is an accepted v1 tradeoff, not an oversight. One file that
  fails to compile doesn't hide every other file's symbols — it's
  silently skipped.
- `shutdown` / `exit` — the ordinary LSP lifecycle; `exit`'s process exit
  code is 0 if `shutdown` was requested first, 1 otherwise, per spec.
- Any other request gets a JSON-RPC `MethodNotFound` (-32601) error;
  any other notification is silently ignored.

## `.div` templates (diagnostics only)

Any document whose path/uri ends in `.div` (`packages/div` — see its own
README for the tag grammar) is not raw Diamond source, so it's never
handed to `diamond_compile` as-is; `lsp/div.c`'s `div_translate` first
turns it into an ordinary Diamond source buffer implementing the same
tag semantics `packages/div/lib/div/compiler.di`'s own `Div.compile_source`
does (a hand-ported C mirror, kept behaviorally in sync by hand — there's
no shared implementation between the two, see that file's own header
comment for the exact grammar and scope cuts), then compiles *that*
through `diagnostics_compute`'s ordinary no-require-bundling path (a
template never `require`s anything of its own). Any resulting diagnostic
gets mapped back through a line-position table `div_translate` builds
alongside the generated source (one entry per generated line, recording
which `.div` source line/column it came from — `{0,0}` for a line with no
single corresponding template position, e.g. the inlined escape-helper
boilerplate, which falls back to anchoring at the template's own start)
instead of through the segment-table machinery every other document's
diagnostics use — there's no `require` bundle here to walk. A tag body
that itself spans multiple lines (a multi-line `<% %>`/`<%= %>`/`<%== %>`)
still maps each of its own physical lines individually, since the
generated code is a verbatim copy of the tag's own text.

Only diagnostics work for `.div` documents — hover, go-to-definition,
document symbols, completion, and workspace symbol search all still
treat them as ordinary (non-)Diamond source and behave accordingly
(mostly: finding nothing, since the raw ERB-tag-mixed-HTML text they'd
otherwise tokenize isn't valid Diamond syntax). Extending any of those to
work *inside* a tag's own embedded expression would need the same kind of
position-mapping div_translate already does for diagnostics, threaded
through each of those handlers separately — a larger, currently
unstarted slice.

This is a server-side capability only — a client still has to actually
send `.div` documents to the server for any of it to run.
`editors/vscode`'s own extension originally only ever did that for the
`diamond` language id, itself only ever assigned to `.di` files
(`package.json`'s `languages` contribution) — a `.div`/`.html.div` file
opened in the editor never got a `didOpen` at all, so this whole feature
silently never fired for anyone using that extension specifically (the
LSP itself, driven directly over stdio, always worked correctly; this
was purely a client-side wiring gap, found shortly after the feature
first shipped). Fixed by giving `.div` its own `diamond-template`
language id (deliberately separate from `diamond`, and with no grammar
of its own — registering it under `diamond` outright would also apply
Diamond's own TextMate grammar to a template's HTML content, which
isn't Diamond syntax) and having `extension.js`'s `didOpen`/`didChange`/
`didClose` accept either language id, while every other provider
(`hover`/`definition`/`documentSymbol`/`completion`/`workspace/symbol`)
stays registered against `diamond` alone, matching the "diagnostics
only" scope above exactly.

Each `.html.div` file is also translated and compiled **in isolation** —
a call to another template's own generated function (a partial, e.g.
`<%== author_books_table_html(books) %>`) reports "undefined function"
here even though the real, divc-compiled output works fine once
`require`d alongside its partial by the app that actually renders it
(`require`'s flat top-level namespace is what makes that work at
runtime — see `packages/div/README.md`). This is a real, known false
positive, not a crash or a wrong location: the reported position still
correctly lands on the unresolved call itself. Resolving it would mean
teaching the LSP which other templates a given one is normally
`require`d alongside, which nothing currently tracks (a `.div` file has
no `require` line of its own to read that from).

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

- **`receiver.method(...)` support beyond the resolvable receiver forms
  above** — an instance-variable receiver (`@item.foo()`), a receiver
  that's itself a call's return value (`make_box().get()`), or a local
  reassigned to a different class later in the same scope (`known_type`/
  `known_type_set` reflect the type as of the local's own declaration,
  not a later reassignment) all still fall back to "not found" rather
  than resolving. A union receiver *is* now resolved, but only for an
  explicit source-level `Dog | Cat` annotation — the compiler does not
  build one from a branching assignment
  (`x = cond ? Dog.new() : Cat.new()`), so that shape stays unresolved
  too; see the hover/definition/documentSymbol bullet above for why.
- **Workspace symbol results for a method name reached through
  `receiver.method(...)`, or for anything needing scope resolution
  beyond a single compiled program's own function/class/local tables.**
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
