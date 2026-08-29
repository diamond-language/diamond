# The cookie-session-backed side of the app: a home page (visit count +
# a short-term note list, both stored in the encrypted session) and a
# CSRF-protected endpoint for adding a note. See app.di for how this is
# wired into the SecurityHeaders -> CookieSession -> RateLimit -> Csrf
# chain -- RateLimit specifically runs *after* CookieSession because its
# own key function (rate_limit_key, app.di) reads the session id
# CookieSession just populated.

def escape_html(text)
  result = text.gsub(Regexp.new("&"), "&amp;")
  result = result.gsub(Regexp.new("<"), "&lt;")
  result = result.gsub(Regexp.new(">"), "&gt;")
  result.gsub(Regexp.new("\""), "&quot;")
end

# Lazily mints a per-session random id -- used only as RateLimit's own
# key (app.di), so requests from the same browser share one rate-limit
# bucket across the life of their session instead of being
# indistinguishable from every other visitor.
def web_visitor_id(request)
  id = request["session"]["visitor_id"]
  if id == nil
    id = SecureRandom.hex(16)
    request["session"]["visitor_id"] = id
  end
  id
end

def render_notes(notes)
  items = []
  def render_one(note)
    items.push("<li>#{escape_html(note)}</li>")
  end
  notes.each(render_one)
  items.join("")
end

# GET / -- renders the current visit count and note list, and embeds
# the request's own CSRF token both as page state and into the inline
# script below. There's no built-in form-body parser (see packages/
# http's own README) for Csrf's synchronizer token to ride along with a
# plain HTML form submission, so the "form" here is a plain input plus
# a tiny script that POSTs a JSON body via fetch() with the token as the
# X-CSRF-Token header instead -- exactly the pattern packages/cookies'
# own README documents as the way to use Csrf from a traditional page.
def web_home(request, context)
  visits = request["session"]["visits"]
  visits = if visits == nil then 0 else visits end
  visits = visits + 1
  request["session"]["visits"] = visits

  notes = request["session"]["notes"]
  notes = if notes == nil then [] else notes end

  csrf_token = Csrf.token(request)

  page = "<!doctype html><html><head><title>Guestbook</title></head><body>" +
    "<h1>Guestbook</h1>" +
    "<p>Visits this session: #{visits}</p>" +
    "<ul>#{render_notes(notes)}</ul>" +
    "<input type=\"text\" id=\"note-text\" placeholder=\"Leave a note\">" +
    "<button id=\"note-submit\">Post</button>" +
    "<script>" +
    "document.getElementById('note-submit').addEventListener('click', function() {" +
    "var text = document.getElementById('note-text').value;" +
    "fetch('/notes', {method: 'POST', headers: {'Content-Type': 'application/json', " +
    "'X-CSRF-Token': '#{csrf_token}'}, body: JSON.stringify({text: text})})" +
    ".then(function() { window.location.reload(); });" +
    "});" +
    "</script></body></html>"

  [200, {"Content-Type": "text/html"}, page]
end

# POST /notes -- protected by Csrf (app.di's chain), so this only ever
# runs once the caller already proved it holds this session's own CSRF
# token. The note list is capped at the 5 most recent entries: it lives
# entirely inside the encrypted session cookie (packages/cookies'
# CookieSession), not a database, so letting it grow without bound would
# grow the cookie itself without bound, not just an in-memory structure.
def web_add_note(request, context)
  parsed = nil
  begin
    parsed = JSON.parse(request["body"])
  rescue error: JSONError
    parsed = nil
  end
  if parsed == nil || parsed["text"] == nil || parsed["text"] == ""
    return [400, {"Content-Type": "application/json"}, JSON.stringify({"error": "text is required"})]
  end
  notes = request["session"]["notes"]
  notes = if notes == nil then [] else notes end
  notes.push(parsed["text"])
  if notes.length() > 5
    notes = notes.drop(notes.length() - 5)
  end
  request["session"]["notes"] = notes
  [200, {"Content-Type": "application/json"}, JSON.stringify({"notes": notes})]
end

def web_handler(request, context)
  if request["path"] == "/notes" && request["method"] == "POST"
    web_add_note(request, context)
  elsif request["path"] == "/" && request["method"] == "GET"
    web_home(request, context)
  else
    [404, {"Content-Type": "text/plain"}, "not found"]
  end
end
