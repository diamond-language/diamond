# Project board authenticated-write benchmark results

Recorded 2026-08-25 on an AMD Ryzen 5 Pro 7535U (6 physical cores, 12
hardware threads) using a release build (`-O3 -DNDEBUG -march=native`). The
Diamond-native load generator ran on the same machine as the server.

The server used six Gremlin workers and the client used six Diamond threads.
Each client performed one real bcrypt login, retained its returned cookie for
100 CRUD iterations, then logged out. That produced 600 CRUD iterations and
7,824 HTTP requests. Each iteration extracted its CSRF token, created/edited/
deleted a unique project and task, and exercised every steady-state route;
project show was requested twice. Across the run all 16 application routes
were exercised. SQLite used WAL mode and a 5,000 ms busy timeout.

Logging was configured through the normal Logger level mechanism at `off`.
Instrumentation and logging calls remained present, but the Logger rejected
all severities before JSON serialization and output. The runner verified that
the server emitted no JSON log lines.

Overall: **7,824 requests in 25.876 seconds, 302.37 requests/second**, with
zero route/status failures.

| route | requests | mean | max |
|---|---:|---:|---:|
| home | 6 | 91.61ms | 175.72ms |
| login form | 6 | 66.48ms | 237.26ms |
| login | 6 | 265.42ms | 416.54ms |
| project new | 600 | 12.51ms | 185.99ms |
| projects index | 600 | 13.08ms | 190.25ms |
| project create | 600 | 28.16ms | 186.89ms |
| project show | 1,200 | 12.07ms | 224.06ms |
| project edit | 600 | 11.98ms | 217.91ms |
| project update | 600 | 27.69ms | 188.14ms |
| task new | 600 | 11.80ms | 205.68ms |
| task create | 600 | 27.09ms | 223.03ms |
| task edit | 600 | 11.66ms | 134.64ms |
| task update | 600 | 25.45ms | 151.06ms |
| task delete | 600 | 28.15ms | 202.09ms |
| project delete | 600 | 25.63ms | 173.98ms |
| logout | 6 | 14.28ms | 24.59ms |

These are single-run local results, not confidence intervals. The load
generator shares the tested CPU and SQLite serializes writers; use the runner
for comparisons on the same machine rather than treating these numbers as a
portable capacity guarantee.
