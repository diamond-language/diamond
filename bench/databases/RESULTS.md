# Database driver benchmark results

Recorded 2026-08-30 with the release build on:

- Linux 7.1.8-200.fc44.x86_64;
- AMD Ryzen 5 Pro 7535U, 6 cores / 12 threads; and
- Podman 5.8.4.

Each server ran alone with one CPU, 768 MiB memory, a 256-PID limit, and a
512 MiB tmpfs data directory. SQLite ran in-process against a fresh `:memory:`
database. The workload used 1,000 rows, 250 warmup point reads, and 5,000
measured operations per category. Values below are arithmetic means of three
fresh-process repetitions. The configuration SHA-256 was
`27b46dd53c5606576bac16786b58d2c8b09258129fcefd0c378e06710a12e551`.

| server | point reads/s | range reads/s | updates/s | connect | seed 1,000 rows |
|---|---:|---:|---:|---:|---:|
| SQLite, system libsqlite3, `:memory:` | 439,043 | 19,691 | 356,004 | 0.10 ms | 2.8 ms |
| PostgreSQL 16.15 | 7,227 | 4,737 | 6,036 | 9.9 ms | 106.9 ms |
| MariaDB 11.4.13 | 6,144 | 3,036 | 5,869 | 1.9 ms | 172.6 ms |
| MySQL 8.4.11 | 6,980 | 3,238 | 4,780 | 3.1 ms | 193.9 ms |

SQLite establishes the cost floor for Diamond's in-process driver path: about
61 times PostgreSQL's point-read rate, 4 times its range-read rate, and 59
times its update rate. Those ratios mostly quantify the work SQLite avoids—no
TCP, separate server scheduling, authentication, wire protocol, or durability
round trip—and are not a like-for-like database-server comparison.

Among the three servers, PostgreSQL was fastest for this single-client Diamond
driver workload: about 4% ahead of MySQL and 18% ahead of MariaDB on
primary-key reads, about 46–56% ahead on the 20-row ordered range query, and
3% ahead of MariaDB / 26% ahead of MySQL on autocommit updates. MySQL was 14%
faster than MariaDB on point reads and 7% faster on range reads, while MariaDB
handled updates about 23% faster than MySQL.

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
