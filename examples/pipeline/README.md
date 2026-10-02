# examples/pipeline

A streaming pipeline whose stages are **untrusted Diamond scripts**, kept
running under supervision. It combines [sandbox mode](../../docs/sandbox.md)
(`--sandbox`, the `DIAMOND_SANDBOX_ALLOW` allow-list, resource budgets) with
[Threads, Channels and a Supervisor](../../docs/threads.md). Where
[`plugins`](../plugins/README.md) runs one-shot plugins and classifies each,
this example keeps a multi-stage pipeline flowing while stages are denied,
run away, or crash.

```text
$ diamond pipeline.di ../../build/diamond
== results
  #1 Ada -> ada=Lovelace#29010
  #2 grace -> grace=Hopper#9147
  #4 Beam -> beam=Erlang#52628
  #6 alan -> alan=Turing#60662
  #8 linus -> linus=Torvalds#16223
== dead letters
  #3 spin at normalize: budget (stages/normalize.di: runtime error: resource limit exceeded)
  #5 beacon at enrich: denied (stages/enrich.di: runtime error: sandbox denies TCPSocket.connect)
  #7 poison at seal: gave up (stages/seal.di: runtime error: uncaught exception: seal failed on poison=? (attempt 3))
== restarts
  normalize: 0
  enrich: 0
  seal: 4
```

## Usage

```text
pipeline.di DIAMOND_BINARY [MANIFEST]
```

`DIAMOND_BINARY` is the `diamond` used to run each stage (a stage runs as
`diamond --sandbox stage.di`, in its own process). `MANIFEST` defaults to
`manifest.json`. Exit status is 0 on completion, 64 for a usage error, and 66
when the manifest can't be read.

## The design

```text
items -> [normalize] -> [enrich] -> [seal] -> results
            |              |           |
            +--------------+-----------+--> dead letters
```

- One **supervised worker thread per stage**, joined by `Channel`s. A worker
  runs its stage once per item as a sandboxed child process.
- `manifest.json` is the host's policy: each stage's script, the capabilities
  it is granted (`allow`) and its instruction and wall-clock budgets. Only
  `enrich` gets `filesystem`; the stage never picks its own trust level.
- Each job carries `{id, item, value, attempt}` through the pipeline.

## What it shows

- **A denial is a verdict, not a crash.** `beacon` makes `enrich` open a TCP
  connection. `network` was never granted, so the sandbox raises
  `SandboxError`; the host recognizes it from stderr and dead-letters the job.
- **Budgets stop what the sandbox cannot.** `spin` makes `normalize` loop
  forever; it touches nothing, so only `DIAMOND_MAX_INSTRUCTIONS` stops it.
- **Crash recovery through the supervisor.** A crash is treated as transient:
  the worker parks the job in a one-slot `inflight` channel and raises, so the
  `Supervisor` restarts it. A restart wipes the worker's heap, but the
  channels survive; the new worker finds the job in `inflight` and retries it
  with `attempt + 1`. `Beam` crashes twice then succeeds; `poison` crashes on
  every attempt and is dead-lettered after `max_attempts` (3). That is the
  four restarts reported for `seal`.
- **Ordered output.** Each stage is one worker, so results come out in input
  order. Dead letters arrive in racy order, so they are sorted by job id.
- **The allow-list is the policy.** `testdata/no_filesystem.json` withholds
  `filesystem` from `enrich`: even the well-behaved item is denied, with no
  change to the stage's code.

## Files

| File | Role |
| --- | --- |
| `pipeline.di` | feeds items, drains results, prints the report |
| `lib/worker.di` | the supervised stage worker: sandboxed run, classify, retry |
| `stages/*.di` | the untrusted stages (`normalize`, `enrich`, `seal`) |
| `manifest.json` | stages, grants, budgets, and the input items |
| `smoke_test.sh` | interpreted and `diamond build` runs against `testdata/` |
