# d6 for VS Code

Language support for [d6](https://rolld6.codes), a gradually typed, object-oriented
language with a bytecode VM:

- Syntax highlighting for `.d6` files and `.div` templates.
- Diagnostics as you type, from the d6 language server.
- Hover, go-to-definition, the Outline view, completion and workspace symbol search
  for functions and classes.
- Debugging with breakpoints and stepping, through VS Code's Run and Debug view.

## Requirements

The extension runs the language server (`d6-lsp`) and debug adapter (`d6-dap`) that come
with d6, so install d6 first:
[getting started](https://rolld6.codes/docs/guide/getting-started.html) has packages for Debian,
Ubuntu and Fedora, a mise install, and building from source. Every install puts `d6-lsp`
and `d6-dap` next to `d6`.

If they are not on your `PATH`, point the extension at them:

| Setting | Default | Meaning |
|---|---|---|
| `diamond.languageServerPath` | `d6-lsp` | The language server |
| `diamond.debugAdapterPath` | `d6-dap` | The debug adapter |

## Debugging

Open a `.d6` file, set a breakpoint in the gutter and press F5 ("Debug d6 File"). Continue,
step over, step into and step out are supported; the Variables pane shows the paused
frame's locals. A `launch.json` entry looks like:

```json
{
  "type": "diamond",
  "request": "launch",
  "name": "Debug d6 File",
  "program": "${file}",
  "args": [],
  "cwd": "${workspaceFolder}"
}
```

## Troubleshooting

If the language server cannot start, VS Code says which path it tried. Its output and
errors are in the "d6 Language Server" output channel, and `d6: Restart Language Server`
restarts it without reloading the window.

What the language server covers is in the
[language server reference](https://rolld6.codes/docs/reference/lsp.html), and the debugger's
limits are in [debugging](https://rolld6.codes/docs/reference/debugging.html). Report problems
at [github.com/diamond-language/d6/issues](https://github.com/diamond-language/d6/issues).
