# Diamond for VS Code

Syntax highlighting for `.di` files — a TextMate grammar (`syntaxes/diamond.tmLanguage.json`)
built directly from `src/lexer.h`/`src/lexer.c`'s own token list, not derived
from or dependent on any other language's grammar. No compiled code, no
`node_modules`, no build step: `package.json` + `language-configuration.json`
+ the grammar file are the entire extension.

This covers syntax highlighting only. For diagnostics, see `lsp/`
(`docs/lsp.md`) — a separate, real Language Server your editor's LSP client
can be pointed at independently of this extension.

## Trying it locally

VS Code loads any folder under its extensions directory that has a
`package.json` with a `contributes` section — no packaging or marketplace
publish needed to try it:

```sh
ln -s "$(pwd)/editors/vscode" ~/.vscode/extensions/diamond-language
```

then reload the VS Code window (`Developer: Reload Window` from the command
palette). Opening any `.di` file should now be highlighted.

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
