# log_viewer

Format newline-delimited JSON logs as readable text.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add log_viewer --registry https://cuts.dilang.tech --version "^0.1.2"
facet update
```

This installs the cut into `cuts/log_viewer/`; load it with `require_cut "log_viewer"`.

## Usage

```sh
diamond app.di | DIAMOND_BIN=diamond cuts/log_viewer/bin/diamond-log
DIAMOND_BIN=diamond cuts/log_viewer/bin/diamond-log logs.ndjson
```

## In a project

Services that log with the [`logger`](https://github.com/diamond-language/diamond/tree/main/packages/logger) cut's `"json"` format (or `RequestLogging`) write one
JSON object per line to standard output. Pipe that through the viewer while developing:

```sh
diamond app.di | DIAMOND_BIN=diamond cuts/log_viewer/bin/diamond-log
# 2026-10-07T23:55:01Z INFO [app] request.completed status=200
```

`diamond-log` prints each entry as `timestamp LEVEL [tag] message`, followed by any extra fields as `key=value`. Keep the raw JSON for
production and aggregation; the viewer is for reading.

## Notes

Set `DIAMOND_BIN` if `diamond` is not on `PATH`. Use `--strict` to exit nonzero when a line is not a JSON log object.
