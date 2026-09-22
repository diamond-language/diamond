# logger

Write leveled text or structured JSON logs.

## Installation

Install the cut at `cuts/logger/` and load it with `require_cut "logger"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## Usage

```ruby
require_cut "logger"

log = Logger.new("myapp")
log.info("starting up")
log.warn("low disk space")
log.error("connection failed: timeout")
```

## Notes

Use `Logger.new(tag, level = "info", output = nil, format = "text")`. Levels are `debug`, `info`, `warn`, `error`, and `off`; choose `"json"` for structured output.
