# Database driver benchmark results

Recorded 2026-08-30 with the release build on:

- Linux 7.1.8-200.fc44.x86_64;
- AMD Ryzen 5 Pro 7535U, 6 cores / 12 threads; and
- Podman 5.8.4.

Each server ran alone with one CPU, 768 MiB memory, a 256-PID limit, and a
512 MiB tmpfs data directory. SQLite ran in-process against a real database
file (`bench.db`) with its default durability settings. The workload used
1,000 rows, 250 warmup point reads, and 5,000 measured operations per
category. Values below are arithmetic means of three fresh-process
repetitions. The configuration SHA-256 was
`b9a1d73f51bf1fc6a162ec195247a1abfaf8d3856171e28cf3d9fd262704e907`.

| server | point reads/s | range reads/s | updates/s | connect | seed 1,000 rows |
|---|---:|---:|---:|---:|---:|
| SQLite, system libsqlite3, file-backed | 95,240 | 15,537 | 73 | 0.19 ms | 13,585.0 ms |
| PostgreSQL 16.15 | 8,027 | 5,038 | 6,482 | 9.5 ms | 95.3 ms |
| MariaDB 11.4.13 | 6,775 | 3,282 | 6,170 | 1.7 ms | 138.9 ms |
| MySQL 8.4.11 | 7,912 | 3,721 | 5,311 | 2.0 ms | 167.6 ms |

With a real file and SQLite's default durability, the picture is not one-sided
the way `:memory:` made it look. SQLite still leads reads by a wide margin —
about 12 times PostgreSQL's point-read rate and 3 times its range-read rate,
reflecting the same lack of TCP, wire protocol, and server scheduling as
before. But on autocommit updates it is now about 88 times *slower* than
PostgreSQL: every update is its own transaction, and SQLite's default
synchronous mode fsyncs the journal on each commit, while the servers batch
that cost behind a network round trip and their own write-ahead logging. Seed
time shows the same effect at 1,000x the row count: 13.6 seconds to insert
1,000 rows one autocommit transaction at a time, versus well under 200 ms for
any of the three servers.

Among the three servers, PostgreSQL was fastest for this single-client Diamond
driver workload: about 1% ahead of MySQL and 18% ahead of MariaDB on
primary-key reads, about 35–54% ahead on the 20-row ordered range query, and
5% ahead of MariaDB / 22% ahead of MySQL on autocommit updates. MySQL was 17%
faster than MariaDB on point reads and 13% faster on range reads, while
MariaDB handled updates about 16% faster than MySQL.

The server measurements are driver-round-trip results, not general database
rankings. They include Diamond/native-client preparation, binding, loopback
TCP, server execution, result decoding, and Diamond allocation. SQLite uses
the same Diamond loops and result handling but executes inside the process.
All rates exclude connection and schema seeding. A different concurrency
level, durability/storage setup, schema, index layout, dataset size, query
mix, server tuning, or connection pool can change both absolute rates and
ordering.

The complete per-run JSON is generated locally as `results.jsonl` and is
ignored by Git. Reproduce and summarize it with the commands in
[README.md](README.md).
