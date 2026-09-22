# log_viewer

Format newline-delimited JSON logs as readable text.

## Installation

Install the cut at `cuts/log_viewer/` and load it with `require_cut "log_viewer"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## Usage

```sh
diamond app.di | DIAMOND_BIN=diamond cuts/log_viewer/bin/diamond-log
DIAMOND_BIN=diamond cuts/log_viewer/bin/diamond-log logs.ndjson
```

## Notes

Set `DIAMOND_BIN` if `diamond` is not on `PATH`. Use `--strict` to exit nonzero when a line is not a JSON log object.
