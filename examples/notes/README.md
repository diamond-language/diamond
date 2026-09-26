# examples/notes

A command-line notebook kept in one SQLite file, using the `SQLite3`
builtin directly: no ORM, just SQL, bind parameters, and a few of SQLite's
own features.

```text
$ echo 'Pick up flour, eggs, and the good butter.' | diamond notes.di add Groceries home errands
added 1
$ echo 'Butter the pan before the eggs go in.' | diamond notes.di add Omelette cooking home
added 2
$ diamond notes.di search butter
   1  Groceries
      Pick up flour, eggs, and the good [butter].
   2  Omelette
      [Butter] the pan before the eggs go in.
$ diamond notes.di list home
   1  2026-08-14  Groceries  [errands, home]
   2  2026-09-20  Omelette  [cooking, home]
```

## Usage

```text
notes [--db FILE] COMMAND [ARGS...]
```

| Command | |
|---|---|
| `add TITLE [TAG...]` | a new note; the body is read from stdin |
| `list [TAG]` | every note, or those with `TAG` |
| `show ID` | one note in full |
| `search QUERY` | full-text search, best matches first ([FTS5 query syntax](https://sqlite.org/fts5.html#full_text_query_syntax): `"exact phrase"`, `a OR b`, `pre*`) |
| `tag ID TAG...`, `untag ID TAG` | add or remove tags |
| `rm ID` | delete a note and its tags |
| `import FILE` | add notes from a JSON array of `{"title", "body", "tags", "created"}` |
| `export` | every note as JSON |
| `stats` | counts by tag and by month |

The database is `--db FILE`, else `$NOTES_DB`, else `notes.db`; it's
created on first use. `$NOTES_NOW` overrides today's date, which the tests
use. Exit status is 0 on success, 1 for an unknown note or rejected input
(including a malformed search), 64 for a usage error, and 66 when a file
can't be read.

## What it shows

- **Migrations** (`lib/store.di`). Each schema version is a list of SQL
  statements, and `PRAGMA user_version` records how far a database has
  come. Opening a version-1 file (from before search existed) creates the
  search index and fills it from the existing notes.
- **Transactions with a block.** `NoteStore#transaction` runs its block
  between `BEGIN` and `COMMIT`, and rolls back if the block exits any other
  way. It uses `ensure` rather than `rescue`, so a `return` or `break`
  from inside the block rolls back as well as an exception does
  (`testdata/transaction_exits.di` checks both). A transaction started
  inside another one joins it, so `add` can be used inside `import`.
- **All-or-nothing import.** `import` validates each entry inside one
  transaction; the first bad entry raises, and the entries before it are
  rolled back with it.
- **Full-text search** with an FTS5 table kept in sync by triggers, ranked
  by `bm25()`, with `snippet()` marking each hit.
- **Bind parameters everywhere** (`?` with an Array), a prepared
  `Statement` reused for each tag, a join for `list TAG`, and `GROUP BY`
  for `stats`.
- **A command dispatcher** matching the arguments against Array patterns
  such as `["add", title, *tags]`.
- **Error mapping**: `NotesError`, `JSONError`, `IOError`, and
  `SQLite3Error` each become a message and an exit status in `main`, and
  the connection is closed in `ensure` whatever happens.

## Test

```sh
bash smoke_test.sh
```

It runs a scripted session (`testdata/session.sh`) through the interpreter
and as a `diamond build` binary, compares both against
`testdata/session.expected`, and checks that a version-1 database is
migrated on open.
