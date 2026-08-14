# Diamond for VS Code

Syntax highlighting for `.di` files — a TextMate grammar (`syntaxes/diamond.tmLanguage.json`)
built directly from `src/lexer.h`/`src/lexer.c`'s own token list, not derived
from or dependent on any other language's grammar — plus live diagnostics
from `diamond-lsp` (`lsp/`, `docs/lsp.md`). No compiled code, no
`node_modules`, no build step: `package.json` + `language-configuration.json`
+ the grammar file + `extension.js` are the entire extension. `extension.js`
is a small hand-rolled LSP client (spawns `diamond-lsp`, frames
`Content-Length` JSON-RPC over its stdio, republishes diagnostics) rather
than a dependency on `vscode-languageclient` — matching `lsp/`'s own
from-scratch, zero-external-dependency convention, and meaning there's
nothing to `npm install` on either side: VS Code's extension host already
bundles Node.

## Diagnostics, hover, go-to-definition, outline, completion, and
## workspace symbol search

1. Build the language server: `make lsp` (from the repo root). This
   produces `build/diamond-lsp`.
2. Either put that binary on your `PATH` under the name `diamond-lsp`, or
   point the extension at it directly: open VS Code settings and set
   `diamond.languageServerPath` to the built binary's absolute path (e.g.
   `/path/to/diamond/build/diamond-lsp`).
3. Open a `.di` file. The extension activates on the `diamond` language,
   spawns the server, and diagnostics appear as you type (Diamond's
   compiler stops at the first error, so at most one diagnostic per file
   at a time). A top-level function name or a class name — at its own
   declaration or anywhere it's used, including across a `require` —
   supports hover (shows its signature), go-to-definition (jumps to the
   declaration, in whichever file it's actually in), and shows up in the
   file's Outline/breadcrumbs view. Only those two identifier kinds
   resolve for hover/definition/outline — see `docs/lsp.md` for exactly
   what is and isn't covered, e.g. none of the three on a method reached
   through `receiver.method(...)`.
4. Completion suggests every top-level function/class in the compiled
   program plus every local variable/parameter actually in scope at the
   cursor (real lexical scoping — a `rescue`-bound name only appears
   inside its own clause, and an outer function's own locals stay
   visible inside a nested closure). Ctrl+T/Cmd+T (Go to Symbol in
   Workspace) searches every top-level function/class across every
   `.di` file under the open folder, not just open documents.

If the server fails to start (wrong path, not built yet), VS Code shows an
error notification with the attempted path; server stderr and lifecycle
events are logged to the "Diamond Language Server" output channel. The
`Diamond: Restart Language Server` command restarts it without reloading
the whole window — useful after rebuilding `diamond-lsp`.

## Trying it locally

VS Code loads any folder under its extensions directory that has a
`package.json` with a `contributes` section — no packaging or marketplace
publish needed to try it:

```sh
ln -s "$(pwd)/editors/vscode" ~/.vscode/extensions/diamond-language
```

then reload the VS Code window (`Developer: Reload Window` from the command
palette). Opening any `.di` file should now be highlighted.

If VS Code is a **Flatpak** install (`flatpak list | grep visualstudio`),
it's sandboxed and never reads `~/.vscode/extensions` at all — symlink into
`~/.var/app/com.visualstudio.code/data/vscode/extensions/diamond-language`
instead, then fully quit and reopen the app (not just reload the window,
since it needs to rescan its extensions directory).

Alternatively, without touching your real extensions directory:

```sh
code --extensionDevelopmentPath="$(pwd)/editors/vscode" .
```

opens a new VS Code window with the extension loaded for that session only.

## What's covered

Keywords (control flow, declarations, visibility/attribute modifiers,
`self`/`super`, `and`/`or`/`not`/`is`), `def`/`class`/`module`/`interface`
names (including operator-overload defs like `def +`/`def ==` and `def
self.name`), string literals with `#{...}` interpolation (including nested
strings inside an interpolation, matching `src/lexer.c`'s own handling),
symbol literals (disambiguated from type-annotation and hash-literal colons
the same way the real lexer does — a colon only starts a symbol when it
isn't glued onto a preceding identifier/`)`/`]`/`}`/`"`), instance
variables, numbers with digit separators, and the builtin type names
(`Int`/`Float`/`String`/`Bool`/`Nil`/`Array`/`Hash`/`Callable`/`Sized`/`Symbol`).

## What isn't

Semantic highlighting (anything that would need actual name resolution —
that's `lsp/`'s job, not a regex grammar's), and no attempt to package or
publish to the VS Code Marketplace.
