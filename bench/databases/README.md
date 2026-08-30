# Database driver benchmark

This benchmark compares Diamond's native database drivers using:

- in-process SQLite using `bench/databases/bench.db`, with its default
  rollback-journal durability;
- in-process SQLite using `bench/databases/bench_wal.db`, tuned to WAL mode
  with `synchronous=NORMAL`;
- PostgreSQL 16 Alpine;
- MariaDB 11.4; and
- Oracle MySQL 8.4.

Versions, ports, credentials, workload sizes, repetitions, and container
limits live in [`config.json`](config.json). The credentials are deliberately
non-secret and exist only for throwaway loopback-bound containers.

## Workload

Each run opens a fresh connection, recreates and seeds the same 1,000-row
table, warms its point-query shape, then measures 5,000 operations in each
category. PostgreSQL, MariaDB, and MySQL run in isolated, resource-limited
Podman services; both SQLite profiles run in the Diamond process against a
real database file, one at SQLite's default durability and one tuned to WAL:

1. primary-key point reads;
2. ordered range reads returning at most 20 rows; and
3. primary-key updates.

Every call uses the same Diamond `?` placeholder API and includes native
client preparation, binding, execution, result decoding, and allocation. The
benchmark is intentionally single-client and sequential: it measures driver
round-trip cost, not maximum database throughput. Connection and seed times
are reported but excluded from operation rates. Container data directories
use tmpfs, keeping host storage differences out of this driver comparison. A
SQLite profile's `pragmas` array in `config.json`, if present, is applied
with `PRAGMA` immediately after connecting; that is how `sqlite_wal` enables
WAL mode.

## Run

```sh
make release
bash bench/databases/run.sh
jq -s -f bench/databases/summarize.jq bench/databases/results.jsonl
```

The harness runs one server database at a time with one CPU, 768 MiB of memory,
and 256 PIDs. It pulls missing images, waits for native readiness probes, captures
the exact server version, performs three repetitions, and removes each
container afterward. Set `KEEP_CONTAINERS=1` to retain the active container
after an interruption, or override `DATABASE_BENCH_CONFIG`,
`DATABASE_BENCH_RESULTS`, or `DIAMOND_BIN`. A partial run can be resumed with,
for example, `DATABASE_BENCH_ENGINES=mysql DATABASE_BENCH_APPEND=1`; engine
names come from `config.json` and are not accepted from untrusted input.

SQLite is a useful local file-backed baseline, not an apples-to-apples server
ranking: it has no loopback TCP, separate server process, network protocol, or
server-side authentication. Its autocommit writes do include the filesystem
and each profile's own transaction/durability behavior — `sqlite` uses
SQLite's default rollback journal, `sqlite_wal` its WAL mode. Do not interpret
MariaDB and MySQL as the same server merely because both use Diamond's
`MySQL` client API; the harness reports them independently.
