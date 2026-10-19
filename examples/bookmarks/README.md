# bookmarks

A bookmark collection that uses six cuts no other example touches: [`database_config`](../../packages/database_config), [`mjolnir`](../../packages/mjolnir), [`network_safety`](../../packages/network_safety), [`multipart`](../../packages/multipart), [`logger`](../../packages/logger) and [`log_viewer`](../../packages/log_viewer).

- `config/database.json` names a connection per environment; `Bookmarks.open` loads one with `DatabaseConfig`.
- Bookmarks are a mjolnir schema. A duplicate URL comes back as a field error, not an exception.
- `network_safety` refuses `localhost` and private addresses before anything is stored.
- `Bookmarks#import` takes a `multipart/form-data` upload whose `list` file has one `url title` per line.
- Every event is one JSON line from `logger`; `log_viewer` formats them back for reading.

It runs in process, with no server and no sockets:

```sh
bash ../../tools/install_local_cuts.sh .
../../build/diamond smoke_test.di
```

`make test-example-apps` runs it with the other examples that need the repository's cuts.
