# A modest push past bench/burn_in/server.di's baseline (20000 sessions/
# worker) -- see bench/burn_in/README.md and docs/gc-generational-design.md
# for the full context. Only the session-cache cap differs from server.di;
# see run_hard.sh for why this pairs with an active RSS watchdog rather
# than trusting a projection from the smaller baseline (a naive linear
# extrapolation from server.di's own documented 4.2-5.1GB @ 20k/worker
# undersold real risk once before -- see the "Bound resource experiments"
# lesson this round is applying).

require "../../packages/gremlin/gremlin"

def build_payload(seed)
  items = []
  index = 0
  while index < 20
    items.push({"id": seed + index, "value": (seed + index) * 3})
    index = index + 1
  end
  items
end

def touch_session(sessions, session_id, path)
  key = "s#{session_id}"
  existing = sessions[key]
  hits = 1
  if existing != nil
    hits = existing["hits"] + 1
  end
  sessions[key] = {"hits": hits, "last_path": path, "payload": build_payload(session_id)}
  hits
end

def handler(request, context)
  if context["sessions"] == nil
    context["sessions"] = {}
    context["next_id"] = 0
  end
  sessions = context["sessions"]

  session_id = context["next_id"]
  next_id = session_id + 1
  max_sessions = 30000
  if next_id >= max_sessions
    next_id = 0
  end
  context["next_id"] = next_id

  path = request["path"]
  hits = touch_session(sessions, session_id, path)

  body = "path=#{path} session=#{session_id} hits=#{hits} sessions_live=#{sessions.length()}"
  [200, {"Content-Type": "text/plain"}, body]
end

def run()
  port = 19421
  threads = 4
  gremlin_serve(port, handler, threads: threads)
end
run()
