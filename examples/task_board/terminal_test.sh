#!/usr/bin/env bash
# Real PTY coverage: launcher, input, persistence, resize and restoration.
#   DIAMOND_BIN=... terminal_test.sh DATABASE
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
database="$1"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
driver="$work/pty_driver"
"${CC:-cc}" -std=c2x -Wall -Wextra -o "$driver" "$here/pty_driver.c"

# The closing steps of every board session: resizing the actual board within its
# usable range changes both dimensions while idle, with shortcuts still visible,
# and no key is sent, because an idle resize must redraw by itself.
closing_resizes() {
  cat <<'SCRIPT'
resize 14 70
expect P project | p switch
fits 14 70
framehas q quit
resize 8 40
expect Terminal too small
resize 24 100
expect P project | p switch
fits 24 100
SCRIPT
}

session() { # reads steps from stdin; $1 is how the session ends
  { echo 'expect Task board'; cat; closing_resizes; cat <<<"$1"; echo restored; } >"$work/script"
  "$driver" "$work/script" bash "$here/board" --db "$database"
}

# A VMIN=0 timed read stays usable afterwards.
cat >"$work/script" <<'SCRIPT'
expect timed out; send a key
send x
exit 0
SCRIPT
"$driver" --timed-read "$work/script" "$DIAMOND_BIN" "$here/terminal_read_test.d6"

session $'send q\nexit 0' <<'SCRIPT'
send p
expect == New project ==
send PTY project\r
expect Created project PTY project
resize 12 60
expect Created project PTY project
fits 12 60
send a
expect == Add task ==
resize 24 100
expect == Add task ==
fits 24 100
send \r
expect title:
send \xc3
sleep 250
resize 12 60
expect == Add task ==
fits 12 60
send \xa9
expect add> \xc3\xa9
send \x7f
expect \e[6;6H\e[?25h
resize 24 100
expect == Add task ==
fits 24 100
send Real terminal task\r
expect Saved #1
send L
expect doing (1)
send e
expect edit> Real terminal task
send !\r
expect Saved #1
send \r
expect Task #1 [doing]
send n
expect Ctrl-S: save
send Remember the benchmark\rSecond line\x13
expect Saved #1
send q
expect Task board
send /
expect search>
send zz_missing
expect No matching tasks
send \e
expect Search: (all tasks)
send /
expect search>
send BENCHMARK\r\r
expect Task #1 [doing]
send n
expect Ctrl-S: save
send \x15Discard this\e
expect Remember the benchmark
send q
expect Task board
send /
expect search>
send \x15\r
expect Search: (all tasks)
SCRIPT

session $'send q\nexit 0' <<'SCRIPT'
send /
expect search>
send SECOND LINE\r\r
expect Task #1 [doing]
send q
expect Task board
send e
expect edit> Real terminal task!
send \e[H
expect \e[6;7H\e[?25h
send \e[C\e[C\e[C\e[C\e[C\e[C\e[C\e[C\e[C\e[C\e[C\e[C\e[C\e[C\e[3~\e[3~\e[3~\e[3~app
expect edit> Real terminal app!
send \e[F
expect \e[6;25H\e[?25h
send \r
expect Saved #1
send \r
expect Task #1 [doing]
send n
expect Ctrl-S: save
send \e[7~\e[3~\e[3~\e[3~\e[3~\e[3~\e[3~Updated
expect Updated line
send \e[A\eOHKeep: 
expect Keep: Remember the benchmark
send \eOF\e[B
expect \e[5;13H\e[?25h
send !\x13
expect Saved #1
send n
expect Ctrl-S: save
send \e[1;5H
expect \e[4;1H\e[?25h
send \e[3~DROP\e
expect Keep: Remember the benchmark
send q
expect Task board
SCRIPT

thirty_lines=''
for number in $(seq -w 0 29); do
  thirty_lines+="line$number"
  [[ "$number" == 29 ]] || thirty_lines+='\r'
done
session $'send q\nexit 0' <<SCRIPT
send /
expect search>
send terminal app\\r\\r
expect Task #1 [doing]
send n
expect Ctrl-S: save
send \\x15$thirty_lines
expect line29
send \\e[1;5H
expect \\e[4;1H\\e[?25h
send \\e[1;5F
expect \\e[20;7H\\e[?25h
resize 12 60
expect \\e[8;7H\\e[?25h
fits 12 60
send \\e[1;5H
expect \\e[4;1H\\e[?25h
send \\e[1;5F
expect \\e[8;7H\\e[?25h
send \\e
expect Keep: Remember the benchmark
send q
expect Task board
SCRIPT

session $'signal 15\nexit 143' </dev/null
session $'send \\x03\nexit 0' </dev/null

listed="$("$DIAMOND_BIN" "$here/app.d6" --db "$database" list)"
[[ "$listed" == '#1 [doing] PTY project / Real terminal app!' ]] || { echo "unexpected list: $listed" >&2; exit 1; }
shown="$("$DIAMOND_BIN" "$here/app.d6" --db "$database" show 1)"
[[ "$shown" == *$'Notes:\nKeep: Remember the benchmark\nUpdated line!' ]] || { echo "unexpected show: $shown" >&2; exit 1; }
echo "task board PTY checks passed"
