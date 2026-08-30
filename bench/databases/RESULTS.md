# Database driver benchmark results

Recorded 2026-08-30 with the release build on:

- Linux 7.1.8-200.fc44.x86_64;
- AMD Ryzen 5 Pro 7535U, 6 cores / 12 threads; and
- Podman 5.8.4.

Each server ran alone with one CPU, 768 MiB memory, a 256-PID limit, and a
512 MiB tmpfs data directory. SQLite ran in-process against a real database
file (`bench.db`), once with its default rollback-journal durability and once
tuned to WAL mode with `synchronous=NORMAL` (`bench_wal.db`) — the standard
production setting, safe against an application crash but not an OS/power
loss. The workload used 1,000 rows, 250 warmup point reads, and 5,000
measured operations per category. Values below are arithmetic means of three
fresh-process repetitions. The configuration SHA-256 was
`1688899ad19c82a33ccd0c09672507584a53a341518d27049e6869a3c0487aff`.

| server | point reads/s | range reads/s | updates/s | connect | seed 1,000 rows |
|---|---:|---:|---:|---:|---:|
| SQLite, file-backed, default journal | 102,776 | 17,617 | 74 | 0.16 ms | 13,471.1 ms |
| SQLite, file-backed, WAL | 210,160 | 18,595 | 25,803 | 0.16 ms | 40.0 ms |
| PostgreSQL 16.15 | 7,951 | 5,088 | 6,769 | 8.7 ms | 97.5 ms |
| MariaDB 11.4.13 | 6,823 | 3,251 | 6,283 | 1.8 ms | 139.2 ms |
| MySQL 8.4.11 | 7,187 | 3,392 | 4,820 | 1.9 ms | 181.4 ms |

With a real file and SQLite's default durability, the picture is not one-sided
the way `:memory:` made it look. SQLite still leads reads by a wide margin —
about 13 times PostgreSQL's point-read rate and 3.5 times its range-read
rate, reflecting the same lack of TCP, wire protocol, and server scheduling
as before. But on autocommit updates it is about 91 times *slower* than
PostgreSQL: every update is its own transaction, and SQLite's default
rollback-journal mode fsyncs twice per commit, while the servers batch that
cost behind a network round trip and their own write-ahead logging. Seed time
shows the same effect at 1,000x the row count: 13.5 seconds to insert 1,000
rows one autocommit transaction at a time, versus well under 200 ms for any
of the three servers.

Switching that same file to WAL mode — a one-line production tuning change,
not a different engine — erases the penalty and then some: updates jump about
348x to 25,803/s, roughly 3.8 times PostgreSQL's rate, because a WAL commit is
a sequential append with only one fsync (skippable entirely under
`synchronous=NORMAL` except at checkpoints) instead of the rollback journal's
allocate-journal/fsync/write/fsync sequence. Reads improve too — WAL readers
don't block behind an in-flight writer — taking point reads to about 2 times
the default-journal rate and roughly 26 times PostgreSQL's; the range-read
gain is smaller (about 6% over the default journal) since that query was
already dominated by the `ORDER BY`. The default rollback journal is a
reasonable choice for a small config file or CLI tool that writes rarely;
nothing that writes under real load should use SQLite's default durability
mode without evaluating WAL first.

Among the three servers, PostgreSQL was fastest for this single-client Diamond
driver workload: about 11% ahead of MySQL and 17% ahead of MariaDB on
primary-key reads, about 50–57% ahead on the 20-row ordered range query, and
8% ahead of MariaDB / 40% ahead of MySQL on autocommit updates. MySQL was
about 5% faster than MariaDB on both point and range reads, while MariaDB
handled updates about 30% faster than MySQL.

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
