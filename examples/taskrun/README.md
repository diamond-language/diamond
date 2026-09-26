# examples/taskrun

A small make-like task runner: tasks declare dependencies and shell
commands in a `Taskfile`, and `taskrun` runs what a target needs, in
dependency order, up to `-j N` at a time.

```text
$ cat Taskfile
all: package docs
clean:
  echo removing build/
compile: clean
  echo compiling main.c
assets: clean
  echo bundling styles
package: compile assets
  echo packing
docs:
  echo writing docs

$ diamond taskrun.di --plan all
1. clean
2. compile
3. assets
4. package
5. docs
6. all
$ diamond taskrun.di -j 4 all
== clean: ok
  $ echo removing build/
  removing build/
...
6 ok, 0 failed, 0 skipped
```

```text
taskrun [-f Taskfile] [-j N] [--time] [--plan] TARGET...
```

A task with no commands just groups its dependencies. Each command runs
through `sh -c`; a task stops at its first failing command. When a task
fails, nothing new starts, running tasks finish, and the rest are reported
as skipped. Exit status: 0 when every task succeeded, 1 when one failed, 2
for a Taskfile error (syntax, unknown task, dependency cycle), 64 for a
usage error, 66 when the Taskfile can't be read.

## What it shows

- **Child processes.** `Job` (`lib/runner.di`) starts each command with
  `Process.spawn`, checks it with `running?()` and `wait()`, and reads its
  stdout and stderr as non-blocking `Process::Stream`s.
- **`IO.poll`.** `Runner#run` waits on every running child's streams at
  once and drains whatever is ready, so a child writing more than a pipe
  buffer holds never blocks (`processes.md` warns that `wait()` alone can
  deadlock). Each task's output is printed in one block when it ends, so
  parallel tasks don't interleave.
- **A dependency graph.** `plan` (`lib/taskfile.di`) orders tasks with a
  depth-first search and names any cycle it finds
  (`loop-a -> loop-b -> loop-c -> loop-a`). Its `visit` helper is a nested
  `def` that calls itself.
- **Parsing with Regexp and structs.** `Task` is a struct; each
  `name: deps` line is matched with a Regexp and destructured.
- **Symbols as states.** A job's outcome is `:running`, `:ok`, or
  `:failed`; the planner marks nodes `:visiting` and `:done`.

## Test

```sh
bash smoke_test.sh
```

This checks ordering and output under the interpreter and as a
`diamond build` binary, failure handling, every error exit, that `-j 3`
runs three 0.4-second tasks in well under 1.2 seconds, and that a child
writing 200 KB of output completes.
