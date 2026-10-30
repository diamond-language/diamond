# Bookmarks

A local reading shelf backed by SQLite and Mjolnir. Add links, search titles,
URLs and tags, filter by tag, import a text list or JSON export, and download
the collection as JSON. The browser uses the same `Bookmarks` collection as
the original in-process example.

From the repository root:

```sh
make debug facet
bash tools/install_local_cuts.sh .
build/diamond examples/bookmarks/web.d6
# Open http://127.0.0.1:18111
```

`web.d6 [PORT [DATABASE [ASSETS_DIRECTORY]]]` defaults to port 18111 and
`bookmarks.db` in the working directory. The asset directory defaults to
`examples/bookmarks/web`, relative to the working directory; pass an absolute
path when launching elsewhere. Data survives restarts. Existing databases gain
an empty tags column without losing bookmarks. Tags are comma-separated,
trimmed, lowercased, and deduplicated.

The app listens on loopback and checks Host/Origin for a single-user local
session. All browser assets are served by Diamond; no Node build is needed.
Ctrl-C closes the HTTP server and database.

Text imports take one `URL title` per line (title defaults to the URL). JSON
exports preserve title and tags; paste the exported array into the import box
to restore it. Imports are best-effort: the result reports added/refused rows;
duplicate URLs and private addresses are refused. There is no metadata fetch.
URL validation resolves public hostnames through `network_safety`, so adding
public links requires DNS access. Search and tag filters run in the browser.

The example also retains its smaller cut demonstrations:

- `Bookmarks.open(environment, log)` loads `config/database.json` through
  `database_config`; the web entry uses an explicit SQLite path instead.
- Mjolnir changesets turn duplicate URLs and invalid attributes into field errors.
- `Bookmarks.import(request)` accepts a multipart file field named `list`.
- `logger` writes one structured event for each add/refusal; `log_viewer`
  formats those events for reading.

Run the original in-process checks from this directory after installing cuts:

```sh
bash ../../tools/install_local_cuts.sh .
../../build/diamond smoke_test.d6
```

From the repository root, `bash examples/web_apps_test.sh` checks real HTTP,
schema migration, import/export, validation, and persistence. Both are included
in `make test-example-apps` on CI.
