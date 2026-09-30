# Cancellation implementation findings

The first cancellation package and job-service implementation exposed two
integration boundaries:

- Gremlin called `exit(0)` when drained, bypassing the service owner's `ensure`.
  The opt-in single-worker `return_after_shutdown` mode now returns so the owner
  can cancel and join its supervised worker.
- Nested rescue directly inside ensure currently emits invalid bytecode. The
  cancellation scope uses a separate `Scope.cleanup()` helper to capture cleanup
  errors instead. This compiler defect remains open; it is not part of the
  cancellation API contract. Reproducer from the repository root:

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

Expected result: `7`. Observed: `runtime error: invalid bytecode` at the nested
rescue end. Do not add a passing test that enshrines that incorrect behavior.
