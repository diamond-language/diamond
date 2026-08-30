# I/O and native services

Choose the guide for the service you are using:

- [Local I/O](local-io.md) — stdout, stdin, files, and file paths.
- [Networking and signals](networking.md) — blocking and non-blocking TCP,
  polling, UDP, signals, and TLS.
- [Databases](databases.md) — SQLite3, PostgreSQL, and MySQL.
- [Time](time.md) — construction, parsing, formatting, timezones, arithmetic,
  comparisons, durations, and monotonic time.
- [Processes](processes.md) — completed commands and live subprocess handles.

All native-resource objects require explicit cleanup where their topic guide
documents `close()`. Use `ensure` when a resource must be released after an
exception.

## Compatibility links

<a id="non-blocking-sockets-tcpserverlisten_nonblocking-socket-iopoll"></a>
- [Non-blocking sockets and `IO.poll`](networking.md#non-blocking-sockets-tcpserverlistennonblocking-socket-iopoll)
<a id="time-constructionparsingformattingarithmeticcomparisons"></a>
- [Time](time.md)
