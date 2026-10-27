# Terminal task board

A small interactive terminal and command-line task board built with Mjolnir and SQLite. Create projects,
add and edit tasks, move them through `todo`, `doing` and `done`, and filter by
project and status.

From the repository root:

```sh
make debug facet
bash tools/install_local_cuts.sh .
build/diamond applications/task_board/app.d6 project Diamond
build/diamond applications/task_board/app.d6 add 1 "Ship a terminal board"
build/diamond applications/task_board/app.d6 move 1 doing
build/diamond applications/task_board/app.d6 list --project 1 --status doing
# #1 [doing] Diamond / Ship a terminal board
build/diamond applications/task_board/app.d6 edit 1 "Ship the board"
build/diamond applications/task_board/app.d6 projects
bash applications/task_board/board
```

The interactive board shows `todo`, `doing`, and `done` side by side. It uses
the same database and validated store as the CLI. Launch it in a POSIX terminal
with Bash and `stty`; use `bash applications/task_board/board --db FILE` to
choose a database. Terminal settings and the previous screen are restored on
exit, errors, and handled signals.

| Key | Action |
| --- | --- |
| Arrows or hjkl | Select a column/task |
| Tab | Next column |
| H / L | Move selected task to previous/next status |
| p | Cycle projects, including all projects |
| P | Create a project |
| a | Add a task to the selected project and column |
| e | Edit selected task's title |
| Enter / Escape | Save / cancel an edit |
| r | Reload tasks and pick up terminal size changes |
| q / Ctrl-C / Ctrl-D | Quit |

On an empty board, press `P` to create a project, then `p` to select it and `a`
to add a task. Validation errors stay visible without discarding the edit.
Long lists scroll with the selection. The board needs at least 60 columns and
10 rows. UTF-8 titles are preserved; cell padding uses byte widths, so non-ASCII
titles may have uneven column alignment. Control characters in stored text are neutralized.
Title editing appends text and supports Backspace; Escape cancels the edit.
The CLI remains available for pipes and scripts.

Usage:

```text
board [--db FILE] project NAME
board [--db FILE] projects
board [--db FILE] add PROJECT_ID TITLE
board [--db FILE] edit TASK_ID TITLE
board [--db FILE] move TASK_ID todo|doing|done
board [--db FILE] list [--project ID] [--status todo|doing|done]
```

Quote names and titles containing spaces. `--db FILE`, recognized before the
command, overrides `BOARD_DATABASE_PATH`; otherwise the database defaults to
`task_board.db` in the working directory. Tables are created if missing;
commands never reset existing data. Each invocation opens and closes its own
connection, so tasks persist between commands.

Entities are frozen structs without persistence methods. The store casts and
validates attributes with Mjolnir changesets. Invalid edits leave the stored
task unchanged; missing projects and duplicate project names become field
errors. Task lists filter in SQL and explicitly preload their projects in a
batch. There are no lazy association queries.

The controller imports its store directly and annotates its `BoardStore`
parameter, as do the CLI helpers. These explicit boundaries let editor
completion and go-to-definition follow store calls across the imported files;
opening the controller alone also resolves its entity types.

Exit codes: 0 for success, 64 for bad usage, IDs or filters, 65 for validation
errors, 66 for a missing task, and 1 for a database error. Diagnostics go to
stderr; results go to stdout.

Run the store, controller, and CLI tests (with isolated temporary databases) using:

```sh
DIAMOND_BIN="$PWD/build/diamond" bash applications/task_board/test.sh
```

When Python 3 is available, this also exercises the real launcher in a
pseudo-terminal, checks persisted keyboard edits and resizing, and verifies
terminal restoration after normal exit and SIGTERM.
