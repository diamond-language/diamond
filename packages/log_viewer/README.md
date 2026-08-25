# packages/log_viewer

A small executable that turns Diamond's newline-delimited JSON logs into a
compact human-readable stream without discarding structured fields.

## Usage

Pipe a running application into the viewer:

```sh
./build/diamond examples/project_board/app.di | DIAMOND_BIN=./build/diamond packages/log_viewer/bin/diamond-log
```

Or format an existing file:

```sh
packages/log_viewer/bin/diamond-log project-board.ndjson
```

Typical output:

```text
2026-08-25T12:00:00-0700 INFO [project_board] request.completed request_id="abc123" method="GET" path="/projects" status=200 duration_ms=0.4821
2026-08-25T12:00:01-0700 DEBUG [project_board] database.query.completed request_id="abc123" method="GET" path="/projects/1" duration_ms=0.071 query_id="q1" phase="completed" operation="query" sql="SELECT * FROM projects WHERE id = ?" bind_count=1 rows=1
```

The four logger metadata fields become the line prefix. Request correlation,
timing, and query fields follow in a stable order. Every other field is then
appended automatically as `key=<JSON value>`, retaining strings, numbers,
booleans, nulls, arrays, and objects without ambiguous stringification.

Malformed or non-object lines are rendered as `[unparsed] <original>` so
mixed streams remain diagnosable. Pass `--strict` to still print the complete
stream but exit nonzero if any line was not a JSON log object.

The shell executable uses `DIAMOND_BIN` when set and otherwise runs `diamond`
from `PATH`. The package's formatter is also available directly with:

```ruby
require_cut "log_viewer"
LogViewer.format_line(json_line)
```
