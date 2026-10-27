# Terminal task board

A small command-line task board built with Mjolnir and SQLite. Create projects,
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
```

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

Exit codes: 0 for success, 64 for bad usage, IDs or filters, 65 for validation
errors, 66 for a missing task, and 1 for a database error. Diagnostics go to
stderr; results go to stdout.

Run the store and CLI tests (with isolated temporary databases) using:

```sh
DIAMOND_BIN="$PWD/build/diamond" bash applications/task_board/test.sh
```
