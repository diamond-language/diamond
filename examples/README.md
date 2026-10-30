# Diamond examples

Start with an application you can use, then follow its data and control flow.
Commands below run from the repository root. Build the runtime and install the
repository's cuts once for examples that use them:

```sh
make debug facet
bash tools/install_local_cuts.sh .
```

| Example | Run | What to try / learn |
| --- | --- | --- |
| [Task board](task_board) | `build/diamond examples/task_board/web.d6` → http://127.0.0.1:18110 | Create a project and task, edit notes, change status. Open `bash examples/task_board/board` against the same database and press `r`: CLI, TUI, and web share Mjolnir validation and frozen entities. |
| [Site checker](site_checker) | `build/diamond examples/site_checker/check.d6 examples/site_checker/urls.txt` | Get `OK … — 200 (… ms)` or `FAIL` lines; add `--json` for scripts. Bounded worker pools, real HTTP, redirects, timeouts, and cancellation. |
| [Bookmarks](bookmarks) | `build/diamond examples/bookmarks/web.d6` → http://127.0.0.1:18111 | Save a link, tag and search it, export JSON, then reimport. Persistent changesets, URL safety, migration, structured logs. |
| [Import desk](job_service) | `build/diamond examples/job_service/service.d6 18090 /tmp/diamond-jobs.sqlite` → http://127.0.0.1:18090 | Queue the sample CSV. See row progress, validation errors, cancel/retry, and contacts surviving restart. Durable jobs, supervisor, deadlines, atomic/idempotent writes. |
| [Project board](project_board) | Follow its setup/compile-views instructions | Authenticated ActiveRecord/Rack application with rendered views; a separate example from the Mjolnir task board. |
| [Notes](notes) | Follow its README | A CLI notebook using SQLite directly, full-text search, and tags, with commands and sample output. |
| [Parallel](parallel) | `build/diamond examples/parallel/parallel.d6` | Deterministic thread/channel/supervisor walkthrough, with optional timing. |
| [Pipeline](pipeline) | Follow its README | Streaming stages and backpressure. |
| [Chat](chat) | Follow its README | WebSocket server and browser clients. |
| [Guestbook](guestbook), [Library](library) | Follow their setup instructions | Smaller HTTP/database examples and server-rendered application structure. |
| [Grades](grades), [Markdown](markdown), [Resolver](resolver), [udiff](udiff) | Follow their READMEs | File processing, parsers, typed records, and command-line tools. |

The web entries above are local single-user examples and serve their own
HTML/CSS/JavaScript; no frontend toolchain is required. Use the explicit asset
directory argument documented by each example when launching outside the repo
root. Ctrl-C stops them cleanly. Each README explains persistent file locations
and how to choose a disposable database.

`make test-example-apps` runs the cut-based application checks on CI, including
the real HTTP web-app checks and the site checker's offline HTTP fixture.
`make test-task-board` separately covers store/controller/CLI and terminal
resize/input behavior. `make test-examples` covers standalone examples;
`make test-job-service` covers the interpreted/compiled lifecycle checks. Tests use disposable
databases and leave user data alone.
