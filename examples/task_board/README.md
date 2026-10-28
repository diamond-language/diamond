# Terminal task board

A small interactive terminal and command-line task board built with Mjolnir and SQLite. Create projects,
add and edit tasks, move them through `todo`, `doing` and `done`, and filter by
project and status.

From the repository root:

```sh
make debug facet
bash tools/install_local_cuts.sh .
build/diamond examples/task_board/app.d6 project Diamond
build/diamond examples/task_board/app.d6 add 1 "Ship a terminal board"
build/diamond examples/task_board/app.d6 move 1 doing
build/diamond examples/task_board/app.d6 list --project 1 --status doing
# #1 [doing] Diamond / Ship a terminal board
build/diamond examples/task_board/app.d6 edit 1 "Ship the board"
build/diamond examples/task_board/app.d6 projects
build/diamond examples/task_board/app.d6 notes 1 "Keep benchmark results here"
build/diamond examples/task_board/app.d6 show 1
bash examples/task_board/board
```

The interactive board shows `todo`, `doing`, and `done` side by side. It uses
the same database and validated store as the CLI. Launch it in a POSIX terminal
with Bash and `stty`; use `bash examples/task_board/board --db FILE` to
choose a database. Terminal settings and the previous screen are restored on
exit, errors, and handled signals.

| Key | Action |
| --- | --- |
| Arrows or hjkl (outside edits) | Select a column/task |
| Tab | Next column |
| H / L | Move selected task to previous/next status |
| p | Cycle projects, including all projects |
| P | Create a project |
| a | Add a task to the selected project and column |
| e | Edit selected task's title |
| / | Search titles, notes, and project names as you type |
| Enter | Open task details; accept a search or title edit |
| n (in details) | Edit multiline notes |
| Ctrl-S (in notes) | Save notes and return to details |
| Escape | Cancel an edit/search, or return from details |
| Ctrl-U (while editing/searching) | Clear the input |
| Left / Right (while editing) | Move the caret; typing inserts at that position |
| Up / Down (in notes) | Move between wrapped rows, retaining the desired column |
| Home / End or Ctrl-A / Ctrl-E | Start / end of the current logical line |
| Ctrl-Home / Ctrl-End | Start / end of the entire text |
| Backspace / Delete | Remove the previous / next UTF-8 code point |
| r | Reload tasks changed by another process |
| q | Return from details, or quit from the board |
| Ctrl-C / Ctrl-D | Quit |

On an empty board, press `P` (or `p`) to open the labelled New project form.
Enter creates and selects the project; press `a` to open the Add task form.
When reopening a database with one project, that project is selected
automatically. Otherwise `p` cycles projects and the All projects view.
Validation errors stay visible in the form without discarding the edit.
Uppercase `P` creates a project; lowercase `p` switches projects. Likewise,
uppercase `H`/`L` moves a task's status, while lowercase `h`/`l` navigates.
Long lists scroll with the selection. The board needs at least 60 columns and
10 rows and redraws automatically when resized, including while idle or editing.
UTF-8 text is preserved and edits never split a code point. Editor
carets assume one terminal column per code point; wide or combining characters
can still misalign visually. Read-only cells use byte-width padding. Control
characters in stored text are neutralized. Text fields show a terminal caret
and scroll to keep it visible. Escape discards the edit.
Search is a literal, case-insensitive substring match within the selected
project. Enter keeps the filter; Escape restores the previous filter and
selection. Use `/`, Ctrl-U, Enter to clear an active filter. Search is local
view state and starts empty on each launch.

Enter opens the selected task's full title and notes, with `j`/`k` or arrows to
scroll. In details, `n` edits notes: move the caret with arrows, type to insert,
and use Backspace or Delete to remove text. Enter inserts a newline, Ctrl-S
saves, and Escape discards the edit. Ctrl-U clears the text. Blank notes are
allowed; notes are limited to 10,000
bytes. Long lines wrap in the detail view. Titles, notes, and status changes
persist in the same SQLite database, including changes made through the CLI.
The CLI remains available for pipes and scripts.

Usage:

```text
board [--db FILE] project NAME
board [--db FILE] projects
board [--db FILE] add PROJECT_ID TITLE
board [--db FILE] edit TASK_ID TITLE
board [--db FILE] move TASK_ID todo|doing|done
board [--db FILE] notes TASK_ID TEXT
board [--db FILE] show TASK_ID
board [--db FILE] list [--project ID] [--status todo|doing|done] [--search TEXT]
```

Quote names and titles containing spaces. `--db FILE`, recognized before the
command, overrides `BOARD_DATABASE_PATH`; otherwise the database defaults to
`task_board.db` in the working directory. Tables are created if missing;
commands never reset existing data. Each invocation opens and closes its own
connection, so tasks persist between commands.
Existing databases automatically gain an empty `notes` column on first open;
the upgrade is transactional and preserves projects, task IDs, and statuses.
Use `notes TASK_ID ""` to clear notes from a script.

Entities are frozen structs without persistence methods. The store casts and
validates attributes with Mjolnir changesets. Invalid edits leave the stored
task unchanged; missing projects and duplicate project names become field
errors. Project/status filters run in SQL and projects are explicitly preloaded
in a batch. Text search uses those rows and associations without extra queries.

`BoardRecords[T]` checks model rows at the Mjolnir read boundary: projects use
`BoardRecords[BoardProject]`, tasks use `BoardRecords[BoardTask]`, and single-row
reads return `T | Nil`. Its `map[U]` checks a projection's callback and returns
`Array[U]` while retaining the repository's model `T`. The CLI project list uses
`map[String]` to format project rows. The underlying Mjolnir schema/query API
remains dynamic; a query returning the wrong model fails at the typed boundary.
The compiler retains those model and projection arguments through assigned
results and chained calls, so `records.all(query)[0]` keeps its model type and
`records.map[String](query, formatter)[0]` keeps `String` for editor completion.

The controller imports its store directly and annotates its `BoardStore`
parameter, as do the CLI helpers. These explicit boundaries let editor
completion and go-to-definition follow store calls across the imported files;
opening the controller alone also resolves its entity types.

Exit codes: 0 for success, 64 for bad usage, IDs or filters, 65 for validation
errors, 66 for a missing task, and 1 for a database error. Diagnostics go to
stderr; results go to stdout.

Run the store, controller, and CLI tests (with isolated temporary databases) using:

```sh
DIAMOND_BIN="$PWD/build/diamond" bash examples/task_board/test.sh
```

When Python 3 is available, this also exercises the real launcher in a
pseudo-terminal, checks idle resize without injected keys, focused forms,
timed input recovery, caret positions, middle-of-text edits, search,
saved/cancelled notes, and resizing, and verifies
terminal restoration after normal exit and SIGTERM.
