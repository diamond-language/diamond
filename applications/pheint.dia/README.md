# pheint.dia

A Diamond application built on Gremlin, Rack, Dials, Div, and the structured
logger.

## Layout

- `app.di` starts the HTTP server.
- `boot.di` loads application dependencies and source files.
- `lib/config` owns environment configuration.
- `lib/controllers` contains request actions.
- `lib/models` is reserved for the domain model.
- `lib/views` contains Div templates.
- `lib/helpers` contains shared application helpers.
- `lib/routes.di` and `lib/middleware.di` wire the request pipeline.

## Run

```sh
cd applications/pheint.dia
bash compile_views.sh
../../build/diamond app.di
```

The server listens on <http://127.0.0.1:18100> by default. `PORT` overrides
the port, `DIAMOND_ENV` selects `development`, `test`, or `production`, and
`LOG_LEVEL` overrides the environment log threshold. Logs are NDJSON and can
be formatted during development with `packages/log_viewer`:

```sh
../../build/diamond app.di | DIAMOND_BIN=../../build/diamond ../../packages/log_viewer/bin/diamond-log
```

Run the direct-dispatch smoke test with:

```sh
DIAMOND_ENV=test ../../build/diamond smoke_test.di
```
