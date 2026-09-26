#!/usr/bin/env bash
# Runs a scripted notes session through the interpreter and as a
# `diamond build` binary, checks it against testdata/session.expected, and
# checks that an old (version 1) database is migrated on open. Set
# DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export work

"$diamond" build notes.di -o "$work/notes" > "$work/build.log"
printf '#!/usr/bin/env bash\nexec "%s" "%s/notes.di" "$@"\n' "$diamond" "$PWD" > "$work/interpreted"
chmod +x "$work/interpreted"

for notes in "$work/interpreted" "$work/notes"; do
  rm -f "$work/session.db"
  NOTES_DB="$work/session.db" NOTES_NOW=2026-09-26 notes="$notes" \
    bash testdata/session.sh > "$work/session.txt"
  diff -u testdata/session.expected "$work/session.txt"
done

# A database from before full-text search existed: version 1, one note.
"$diamond" -e '
db = SQLite3.open(ARGV[0])
db.execute("CREATE TABLE notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT NOT NULL, created TEXT NOT NULL)")
db.execute("CREATE TABLE tags (note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE, name TEXT NOT NULL, PRIMARY KEY (note_id, name))")
db.execute("INSERT INTO notes (title, body, created) VALUES (?, ?, ?)", ["Old", "Written before search existed.", "2026-01-02"])
db.execute("PRAGMA user_version = 1")
db.close()' "$work/old.db" > /dev/null
"$work/notes" --db "$work/old.db" search existed | grep -q "Old"
"$diamond" -e 'puts(SQLite3.open(ARGV[0]).query("PRAGMA user_version")[0]["user_version"])' "$work/old.db" | grep -qx 2

# A return or break out of a transaction block rolls it back.
"$diamond" testdata/transaction_exits.di | head -4 | tr '\n' ' ' | grep -qx 'left early broke out 0 1 '

echo "notes smoke test passed"
