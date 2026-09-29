# examples/plugins

A host that runs untrusted Diamond scripts as sandboxed plugins, each in its
own subprocess, with the host deciding per plugin what it's allowed to touch.

```text
$ diamond host.di $(which diamond)
wordcount
  ok             words=9 vowels=11 longest=quick
peek (untrusted)
  sandbox_denied plugins/peek.di: runtime error: sandbox denies File.open
peek (trusted reader)
  ok             first line: Only a plugin the host trusts with filesystem access can read this file.
tidy (untrusted)
  ok             no filesystem access; skipping plugins/data/notice.txt
spinloop
  resource_limit plugins/spinloop.di: runtime error: resource limit exceeded
```

`peek.di` runs twice, byte-for-byte the same file both times — once denied,
once allowed — because trust in this design is a property of *the run the
host chooses to grant*, described in `manifest.json`, never something the
plugin itself asks for or can affect.

## Usage

```text
host.di <path-to-diamond-binary>
```

The host needs its own interpreter's path to spawn plugins with, since a
plugin runs as `<that path> --sandbox <plugin path> [arg]`, not as a call
back into the host's own process.

Edit `manifest.json` to add a plugin: `path` and `arg` are what to run and
what to pass it, `allow` is the list of sandbox capabilities to grant just
that one entry (`filesystem`, `network`, `database`, `subprocess` — see
[docs/sandbox.md](../../docs/sandbox.md)), and `max_instructions`/
`max_wall_ms` set its `DIAMOND_MAX_INSTRUCTIONS`/
`DIAMOND_MAX_WALL_MILLISECONDS` budget.

## What it shows

- **`--sandbox` and `DIAMOND_SANDBOX_ALLOW`.** `peek.di` tries `File.open`
  unconditionally; denied by default, it succeeds only for the manifest
  entry that lists `filesystem` in its `allow` array. The host sets
  `DIAMOND_SANDBOX_ALLOW` only in the child's own environment (via `env`,
  never a shell), so one plugin's grant never leaks to another's process.
- **Resource limits as a second, independent axis.** `spinloop.di` never
  touches the filesystem or network at all — `--sandbox` alone would let it
  spin forever — so it's `DIAMOND_MAX_INSTRUCTIONS` that stops it. Sandbox
  mode answers "what can this touch"; resource limits answer "how much can
  it do," and a real host needs both.
- **Two ways a plugin can handle being denied.** `peek.di` doesn't catch
  `SandboxError` and crashes; `tidy.di` does (`rescue error: SandboxError`,
  straight from docs/sandbox.md's own example) and reports a friendly
  message instead. The host treats both as legitimate outcomes — a denial
  isn't a bug in the host, however the plugin chose to handle it — and
  reserves "crashed" for anything else.
- **Subprocess isolation as real defense in depth.** Every plugin is a
  separate OS process (`Process.run`), not just a separate check inside one
  process. A crashed or malicious plugin can't corrupt the host's own
  memory; the host only ever sees its exit code, stdout, and stderr.
- **Classifying a crash from the outside.** The host never sees the
  plugin's raised exception as an object — only the text `diamond`'s own
  uncaught-error reporter wrote to stderr — so `classify()` pattern-matches
  on that text (`"sandbox denies"`, `"resource limit exceeded"`) the same
  way a real process-supervision tool would.
- **A plain JSON manifest** (`manifest.json`, read with `JSON.parse`) as
  the host's whole policy, and a `struct` (`PluginResult`) for what running
  one plugin comes back with.

## Test

```sh
bash smoke_test.sh
```

Runs the host under the interpreter and as a `diamond build` binary, and
checks that all five manifest entries come back with the status their own
design intends.
