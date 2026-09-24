# Everything the server needs except starting it, so app.di stays a few lines.
require_cut "gremlin"
require_cut "websocket"
require "./lib/room"
require "./lib/bot"
require "./lib/page"

def chat_max_text() = 500

def chat_send(ws, event)
  websocket_try_send_text(ws, JSON.stringify(event))
end

# A display name is 1-24 characters with no control characters; surrounding
# spaces are trimmed. Returns nil for anything else.
def chat_clean_name(value)
  unless value is String then return nil end
  name = value.strip()
  if name.length() < 1 || name.length() > 24 then return nil end
  ok = true
  name.chars().each() do |ch|
    if ch.ord() < 32 || ch.ord() == 127 then ok = false end
  end
  if ok then name else nil end
end

def chat_parse(data)
  begin
    event = JSON.parse(data)
    if event is Hash then event else nil end
  rescue error: JSONError
    nil
  end
end

# One chat line from `name`: commands go to the bot (or are answered here when
# they need server state), everything else is broadcast.
def chat_say(ws, name, text)
  if text.start_with?("/stats")
    chat_send(ws, {"type": "bot", "text": "#{Room.members().length()} online; bot restarts: #{Bot.restarts()}"})
  elsif text.start_with?("/")
    Room.broadcast({"type": "message", "name": name, "text": text})
    unless Bot.ask(name, text)
      chat_send(ws, {"type": "error", "text": "The bot is busy; try again shortly."})
    end
  else
    Room.broadcast({"type": "message", "name": name, "text": text})
  end
end

# Runs on the connection's own fiber for as long as the socket is open. The
# first message must be {"type": "join", "name": ...}; later ones are
# {"type": "say", "text": ...}.
def chat_session(ws)
  name = nil
  begin
    loop do
      message = ws.receive()
      break if message == nil
      event = chat_parse(message["data"])
      if event == nil
        chat_send(ws, {"type": "error", "text": "Messages must be JSON objects."})
      elsif name == nil
        candidate = chat_clean_name(event["name"])
        if event["type"] != "join" || candidate == nil
          chat_send(ws, {"type": "error", "text": "Join first with a name of 1-24 characters."})
        elsif Room.taken?(candidate)
          chat_send(ws, {"type": "error", "text": "#{candidate} is already here; pick another name."})
        else
          name = candidate
          Room.join(ws, name)
          chat_send(ws, {"type": "welcome", "name": name, "history": Room.history()})
          Room.broadcast({"type": "joined", "name": name, "members": Room.names()})
        end
      elsif event["type"] == "say" && event["text"] is String
        text = event["text"].strip()
        if text.length() > chat_max_text()
          chat_send(ws, {"type": "error", "text": "Messages are limited to #{chat_max_text()} characters."})
        elsif text.length() > 0
          chat_say(ws, name, text)
        end
      end
    end
  rescue error: IOError
    nil
  end
  if name != nil && Room.leave(ws)
    Room.broadcast({"type": "left", "name": name, "members": Room.names()})
  end
end

def chat_handler(request, context)
  path = request["path"]
  if path == "/ws" && websocket_upgrade_request?(request)
    chat_session(websocket_accept(context["gremlin_connection"], request))
    nil
  elsif path == "/" && request["method"] == "GET"
    [200, {"Content-Type": "text/html; charset=utf-8"}, chat_page()]
  else
    [404, {"Content-Type": "text/plain"}, "not found"]
  end
end

def chat_tick(context)
  Bot.drain()
end
