# Project board authenticated-write benchmark

Exercises every route in the real project-board HTTP stack with a
Diamond-native threaded client. Each client visits home, logs in once, retains
its real session cookie across all iterations, and logs out once at the end.
Every iteration exercises project index/new/create/show/edit/update, task
new/create/edit/update/delete, and project delete. The client extracts the
real CSRF token and generated record IDs from responses—there is no
benchmark-only authentication shortcut. Keeping authentication outside the
steady-state loop separates bcrypt cost from authenticated CRUD throughput.

Application and ActiveRecord/Arel query logging use the Logger's `off` level,
set in each isolated worker's context before its Logger is constructed. All
instrumentation and log calls stay in place; the Logger no-ops events because
every severity is below its level, avoiding JSON serialization and log I/O
without maintaining a special benchmark code path. SQLite runs in WAL mode
with a five-second busy timeout to serialize concurrent writers without
treating ordinary lock contention as failed requests.

The default runner starts six Gremlin server workers and six Diamond load
threads, each performing 100 CRUD iterations (7,824 requests total, including
one login/logout lifecycle per client):

```sh
make release
bash bench/project_board_http/run.sh
```

Configure it with `SERVER_THREADS`, `LOAD_THREADS`, and `ITERATIONS`. The load
driver is itself an ordinary Diamond script and can also target an already
running server directly:

```sh
build/diamond bench/project_board_http/load.di 12 250 http://127.0.0.1:19620
```

Arguments are load threads, journeys per thread, and base URL. Output is one
JSON object containing total throughput plus per-route request counts, mean
latency, and maximum latency. Any unexpected HTTP status fails its worker and
the run; the shell runner also fails if the supposedly disabled server emits
JSON logs.
