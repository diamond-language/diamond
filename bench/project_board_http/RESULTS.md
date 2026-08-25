# Project board authenticated-write benchmark results

Recorded 2026-08-25 on an AMD Ryzen 5 Pro 7535U (6 physical cores, 12
hardware threads) using a release build (`-O3 -DNDEBUG -march=native`). The
Diamond-native load generator ran on the same machine as the server.

The server used six Gremlin workers and the client used six Diamond threads.
Each client performed 100 complete journeys, for 600 journeys and 10,200 HTTP
requests. Every journey performed a real bcrypt login, retained its returned
cookie, extracted its CSRF token from the project form, created/edited/deleted
a unique project and task, and logged out. All 16 routes were exercised;
project show was requested twice per journey. SQLite used WAL mode and a
5,000 ms busy timeout.

Logging was configured through the normal Logger level mechanism at `off`.
Instrumentation and logging calls remained present, but the Logger rejected
all severities before JSON serialization and output. The runner verified that
the server emitted no JSON log lines.

Overall: **10,200 requests in 76.578 seconds, 133.20 requests/second**, with
zero route/status failures.

| route | requests | mean | max |
|---|---:|---:|---:|
| home | 600 | 29.41ms | 393.63ms |
| login form | 600 | 29.26ms | 355.44ms |
| login | 600 | 214.59ms | 649.65ms |
| project new | 600 | 23.60ms | 545.75ms |
| projects index | 600 | 25.60ms | 418.69ms |
| project create | 600 | 40.02ms | 422.37ms |
| project show | 1,200 | 26.00ms | 373.05ms |
| project edit | 600 | 26.20ms | 388.54ms |
| project update | 600 | 37.45ms | 367.17ms |
| task new | 600 | 31.17ms | 359.23ms |
| task create | 600 | 38.86ms | 390.68ms |
| task edit | 600 | 30.60ms | 413.13ms |
| task update | 600 | 39.90ms | 540.17ms |
| task delete | 600 | 42.33ms | 485.29ms |
| project delete | 600 | 42.15ms | 407.67ms |
| logout | 600 | 45.89ms | 375.94ms |

These are single-run local results, not confidence intervals. The load
generator shares the tested CPU, bcrypt dominates login, and SQLite serializes writers; use the runner
for comparisons on the same machine rather than treating these numbers as a
portable capacity guarantee.
