# Find a pricing bug with the debugger

Three items at 12 cents should cost 36 cents. `debug_me.di` deliberately
calculates 15. Work through the pause to find out why, then compare your fix
with `solution.di`.

## Terminal walkthrough

From the repository root:

```sh
make debug
build/diamond examples/debug_me/debug_me.di
```

The `debugger()` call pauses inside `line_total` after calculating `total`.
The output includes:

```text
locals:
  price = 12
  quantity = 3
  total = 15
(press Enter to continue)
```

The inputs are correct, so inspect the calculation just above `debugger()`:
`price + quantity` adds the quantity instead of multiplying by it. Press Enter
to resume; the program prints `Total: 15 cents` and exits.

Change `+` to `*`, run again, and confirm that the paused `total` is now 36.
Remove `debugger()` once satisfied. The supplied solution does both:

```sh
build/diamond examples/debug_me/solution.di
# Total: 36 cents
```

`breakpoint()` is an alias for `debugger()`. These terminal pauses print the
current locals and accept Enter to continue; they do not offer an expression
prompt or terminal stepping commands. EOF also resumes immediately, which is
useful for automated runs:

```sh
build/diamond examples/debug_me/debug_me.di </dev/null
```

## Editor walkthrough

Build the adapter with `make dap` and follow the
[VS Code debugger setup](../../editors/vscode/README.md#debugging-dap).
Ensure the editor inherits `DIAMOND_BIN` pointing to the absolute path of
`build/diamond`, or has `diamond` on its PATH. Set `diamond.debugAdapterPath`
to the absolute path of `build/diamond-dap`.

Open `debug_me.di` and set a gutter breakpoint on `total = price + quantity`.
Launch **Debug Diamond File**. The breakpoint pauses before the calculation;
inspect `price` and `quantity` in Variables. Use Step Over to reach the
explicit `debugger()` pause and inspect `total`. The Call Stack shows
`line_total` and its caller. Continue to see the incorrect printed total.

You can use Step Into, Step Over, and Step Out, and add or remove gutter
breakpoints during the session. Only the currently paused, innermost frame
has inspectable locals. To experiment using gutter breakpoints alone, remove
the explicit `debugger()` call and restart. See [debugging details](../../docs/debugging.md)
for stepping limitations, including tail calls and spawned threads.

## Smoke test

```sh
bash examples/debug_me/smoke_test.sh
```

The test checks the diagnostic locals and continuation with both Enter and
EOF, then checks the corrected result. It runs interpreted and compiled
versions; the intentionally wrong calculation is part of the lesson.
