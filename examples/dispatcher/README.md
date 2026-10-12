# examples/dispatcher

Routing work across bounded queues without blocking on any one of them, a
worker that drains several queues with a priority cancel channel, a supervisor
that gives up on a crash loop instead of retrying forever, and a `case` that
needs one branch for a whole family of classes.

```text
$ diamond dispatcher.di
== routing
  job 1 -> primary
  job 2 -> primary
  job 3 -> overflow
  ...
  job 6 shed
  job 7 shed
queued: primary 2, overflow 3, shed [6, 7]
...
== crash loop
  join raised SupervisorError
  failed?: true
  restarts before giving up: 2
  add_child refused after failure
```

`expected.txt` has the complete output, which is the same on every run.

## What it shows

1. **`Channel.select_send`.** `Channel.select_send([primary, overflow], job,
   deadline)` sends to the first channel in the array with room and returns its
   index. A deadline in the past means "never wait", so when both queues are
   full it returns `nil` at once and the program sheds the job. Closed channels
   are skipped.
2. **`Channel.select` with a priority.** One thread drains both queues with
   `Channel.select([cancel, primary, overflow])`. The first ready channel in
   array order wins, so listing `cancel` first means a cancellation is never
   starved by a busy queue. The example also shows a pending cancel beating a
   ready job.
3. **`Supervisor#restart_policy`.** A child that always crashes would restart
   forever. With `{"max_restarts": 2, "within_ms": 5000, "backoff_ms": 5}` the
   supervisor gives up on the third crash: `failed?` is `true`, `join` raises
   `SupervisorError`, and `add_child` is refused afterwards.
4. **Group coverage in `case`.** `when Failure` covers `Shed` and `Crashed`
   together, so a `case` over `Done | Shed | Crashed` needs two branches. Delete
   the `Failure` branch and the compiler reports `missing: Shed, Crashed`.

Worker functions live in `lib/dispatch.di`. Like the `parallel` example, they
are top-level defs that capture nothing, because each thread runs one in its
own VM.

## Run it

```sh
make debug
bash examples/dispatcher/smoke_test.sh
```

The smoke test runs the program interpreted and as a `diamond build` binary
and compares both with `expected.txt`.
