# Cancellation implementation findings

The first cancellation package and job-service implementation exposed two
integration boundaries:

- Gremlin called `exit(0)` when drained, bypassing the service owner's `ensure`.
  The opt-in single-worker `return_after_shutdown` mode now returns so the owner
  can cancel and join its supervised worker.
- Nested rescue directly inside ensure exposed a VM unwind-state defect: one
  pending result/exception slot could not preserve enclosing cleanup state.
  PR #6 fixed this with stacked handler state and GC roots. The cancellation
  scope retains its separate `Scope.cleanup()` helper to capture cleanup errors
  without replacing the body's exception. The original reproducer now passes:

```sh
build/diamond -e 'def repro()
 begin
  7
 ensure
  begin
   1
  rescue error
   2
  end
 end
end
repro()'
```

Expected and observed result after the fix: `7`. Nested ensure regressions cover
return values, exceptions, non-local exits, and GC during cleanup.
