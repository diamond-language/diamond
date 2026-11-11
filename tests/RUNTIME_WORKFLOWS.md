# Runtime workflow coverage

These cases complement the existing method-level and language-feature tests
with interactions and failure recovery. They are Diamond tests and require no
Pheint, Skindicate, application fixtures or external services.

| Case | Contracts exercised |
|---|---|
| `runtime_workflow_json` | Recover from malformed documents; nested round trips, UTF-8, escapes, false/nil and independent decoded objects |
| `runtime_workflow_collections` | Deletion and rehash preservation, arbitrary-precision keys, nil versus missing, nested type guards without partial mutation |
| `runtime_workflow_numbers` | Signed overflow promotion/demotion, wide division/shifts/bit operations/modulo, recovery after division by zero |
| `runtime_workflow_control` | Ordered side effects across spread and keyword arguments, safe-navigation short circuit, cleanup during return, retry and captures |
| `runtime_workflow_regex` | Invalid-pattern recovery, multibyte/optional captures, failed-match state, expression reuse and JSON serialization |
| `runtime_workflow_dispatch` | Included-module calls through subclass overrides, escaped self-capturing closures and dynamic method invocation |
| `sqlite3_workflow_recovery` | Reuse prepared statements after bind/uniqueness failures; rollback and subsequent commit without stale bindings |
| `process_workflow_streams` | Drain both large stdout/stderr pipes, preserve failure status and bytes, isolate per-child environment and feed large stdin |
| `channel_workflow_payloads` | Structured payload copies across a thread, independent mutation, Unicode/false/nil preservation and drain-before-close |
| `file_workflow_atomic` | Binary publish/copy, refusal to overwrite, rename replacement and temporary-file cleanup on failed publish |

Each is an ordinary `tests/cases/*.d6` case with a golden result and contextual
failure messages, so the existing case runner, minimal-build filtering and
sanitizer runs pick it up. SQLite is in-memory; file/process fixtures use private
temporary paths. No network, performance threshold, or pinned source line is
needed.

`make test` additionally runs `tests/runtime_workflows.sh`: all ten cases in the
interpreter, first-call JIT, JIT with major-GC stress, and JIT with minor-GC
stress. Private bytecode copies then verify a cold miss and a hot hit under
JIT with the same expected results. This adds 60 executions across modes.
JIT is enabled rather than required on unsupported architectures; native calls
can still legitimately fall back to the interpreter.

`make test-runtime-workflows` adds standalone binaries for control flow, JSON
and SQLite, each with JIT disabled and enabled (66 executions total). The full
integration target includes this check. It compares semantic results, not JIT
function counts, and places a 30-second bound on each execution to catch
deadlocks.

For an already-built runtime:

```sh
bash tests/runtime_workflows.sh
bash tests/runtime_workflows.sh --aot
```

This is a behavioral coverage expansion, not an instrumented line/branch
coverage measurement. It does not replace differential fuzzing, sanitizer or
thread-race testing.
