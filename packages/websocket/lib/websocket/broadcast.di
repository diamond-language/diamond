# A safe way to write to *another* connection's WebSocket from a
# different connection's own fiber -- the shape any fan-out broadcast
# (a chat room relaying one user's message to everyone else in it,
# say) fundamentally needs, and genuinely unsafe to do with
# WebSocketConnection#send_text/#send_binary directly.
#
# Why: gremlin_worker's own accept/poll/resume loop (packages/gremlin/
# lib/gremlin/server.di) resumes a suspended fiber only when *that
# fiber's own* connection becomes ready again -- each `connections`
# entry pairs one fiber with one conn, and readiness is checked and
# routed per entry. If sending a message to connection B's socket ever
# blocked (WouldBlockError) while running on connection A's own fiber
# (exactly what fan-out means: A's fiber is the one iterating every
# room member, including B, to relay A's own message), the ordinary
# yield-and-retry NonblockingConnection#write already does would
# suspend *A's* fiber -- but gremlin_worker would only ever resume it
# once *A's own* socket (not B's, the one actually blocked on) becomes
# writable again. Nothing would ever wake that suspended write, and A's
# connection would wedge forever while still showing up "alive" in
# gremlin_worker's own `connections` list, a permanent leak, not just a
# stall.
#
# This sidesteps the whole problem by never yielding on B's behalf at
# all: it writes directly to the raw socket (bypassing
# NonblockingConnection#write's retry-loop entirely) and, if that would
# block, gives up immediately rather than trying to wait correctly.
# Since RFC 6455 framing has no way to recover from a *partially* sent
# frame (it would desync the peer's own frame parser for the rest of
# the connection's life), a message either goes out completely or the
# connection is closed outright instead -- the same tradeoff a real IRC
# server makes ("Excess Flood" disconnects a client that can't keep up
# rather than buffering for it indefinitely or blocking everyone else
# on its behalf).
#
# Returns true if `text` was fully delivered, false if `ws` was closed
# instead (a caller doing fan-out should drop `ws` from whatever
# membership list it came from when this returns false).
#
# Rescues IOError as well as WouldBlockError: `ws.closed?()` above only
# catches a connection *this* function (or WebSocketConnection#close/
# #handle_close_frame) already knows is closed -- it can't see a
# connection whose underlying raw socket was closed some other way
# (confirmed directly: gremlin_worker's own per-connection crash
# recovery, packages/gremlin/lib/gremlin/server.di's resume_if_ready,
# closes a failed connection's raw NonblockingConnection straight away
# on an uncaught exception, with no way to also reach into whatever
# app-level WebSocketConnection was wrapping it). Writing to an
# already-closed socket raises a plain IOError ("socket is closed"),
# not WouldBlockError -- both are treated identically here, since
# either way the frame can't be delivered and `ws` needs to come out of
# the caller's own membership list.
def websocket_try_send_text(ws, text)
  if ws.closed?()
    return false
  end
  frame = websocket_encode_frame(websocket_opcode_text(), text)
  socket = ws.raw_socket()
  remaining = frame
  begin
    while remaining.length() > 0
      written = socket.write(remaining)
      remaining = remaining.slice(written, remaining.length() - written)
    end
    true
  rescue error: WouldBlockError | IOError
    ws.force_close()
    false
  end
end
