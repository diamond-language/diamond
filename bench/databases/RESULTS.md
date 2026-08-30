# Database driver benchmark results

Recorded 2026-08-30 with the release build on:

- Linux 7.1.8-200.fc44.x86_64;
- AMD Ryzen 5 Pro 7535U, 6 cores / 12 threads; and
- Podman 5.8.4.

Each server ran alone with one CPU, 768 MiB memory, a 256-PID limit, and a
512 MiB tmpfs data directory. The workload used 1,000 rows, 250 warmup point
reads, and 5,000 measured operations per category. Values below are arithmetic
means of three fresh-process repetitions. The configuration SHA-256 was
`1682b267329b7253c59ae338fe05aee7b21ba3fe104abb9ff97ae665b0ae7ff4`.

| server | point reads/s | range reads/s | updates/s | connect | seed 1,000 rows |
|---|---:|---:|---:|---:|---:|
| PostgreSQL 16.15 | 8,011 | 5,184 | 6,609 | 8.7 ms | 99.3 ms |
| MariaDB 11.4.13 | 6,871 | 3,283 | 6,230 | 1.9 ms | 139.3 ms |
| MySQL 8.4.11 | 7,298 | 3,465 | 4,796 | 3.0 ms | 181.2 ms |

PostgreSQL was fastest for this single-client Diamond driver workload: about
10% ahead of MySQL and 17% ahead of MariaDB on primary-key reads, about 50–58%
ahead on the 20-row ordered range query, and 6% ahead of MariaDB / 38% ahead
of MySQL on autocommit updates. MySQL was 6% faster than MariaDB on point
reads and 6% faster on range reads, but MariaDB handled updates about 30%
faster than MySQL.

These are driver-round-trip results, not general database rankings. They
include Diamond/native-client preparation, binding, loopback TCP, server
execution, result decoding, and Diamond allocation. They exclude container
startup and schema seeding from operation rates. A different concurrency
level, durability/storage setup, schema, index layout, dataset size, query
mix, server tuning, or connection pool can change both absolute rates and
ordering.

The complete per-run JSON is generated locally as `results.jsonl` and is
ignored by Git. Reproduce and summarize it with the commands in
[README.md](README.md).
