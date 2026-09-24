# examples/chat

A realtime chat room in the browser: WebSockets on
[`gremlin`](../../packages/gremlin/README.md) and
[`websocket`](../../packages/websocket/README.md), plus a chat bot on its own
thread that talks to the server over `Channel`s and is restarted by a
`Supervisor` when it crashes.

Its cuts come from the public registry, not this repository: `diamond.cut`
names them and `facet.lock` pins the exact archives.

## Run it

```sh
cd examples/chat
facet install          # installs the locked cuts into cuts/
diamond app.di         # set CHAT_PORT to change the default 18095
```

Open http://127.0.0.1:18095 in two browser windows, pick names, and chat.
Commands:

| Command | Handled by |
|---|---|
| `/roll [sides]` | the bot thread (2-1000 sides, default 6) |
| `/time` | the bot thread |
| `/crash` | the bot thread raises; the supervisor restarts it |
| `/stats` | the server: members online and bot restarts |
| `/help` | the bot thread |

## How it fits together

- **One gremlin worker, many fibers.** `gremlin_serve(port, chat_handler, 1,
  0.05, chat_tick)` keeps every connection on one thread, so `Room`
  (`lib/room.di`) can hold all of them in class variables. Each connection's
  handler (`chat_session` in `boot.di`) parks its own fiber in `ws.receive()`.
- **Fan-out without stalls.** `Room.broadcast` sends with
  `websocket_try_send_text`, which never blocks the sender's fiber. A member
  whose socket can't take a whole frame is closed and announced as having left.
- **A bot on another thread.** Threads share no memory, so the bot
  (`lib/bot.di`) receives commands on one `Channel` and sends replies on
  another. The server never blocks on it: commands go in with `try_send`, and
  gremlin's 50 ms tick drains replies with `try_receive`.
- **Crash and restart.** `/crash` raises inside the bot. Its `Supervisor`
  starts a fresh attempt with the same channels, and the next tick announces
  the restart using `restart_count` and `last_error`.
- **Clean shutdown.** gremlin exits on SIGINT/SIGTERM once connections drain.

The page (`lib/page.di`) is plain HTML and JavaScript. It renders every value
with `textContent`, so chat text can't inject markup.

## Test

```sh
python3 smoke_test.py
```

This starts the real server and uses a minimal WebSocket client (Python's
standard library has none) to cover joining, duplicate names, fan-out, bot
replies, a crash and restart, `/stats`, history for late joiners, departure
notices, and a clean SIGTERM exit.

## Limits

- One worker thread. Scaling across threads would need a shared store for
  membership and fan-out, since workers share no memory; see gremlin's
  `on_tick` notes.
- There is no authentication; names are first come, first served.
