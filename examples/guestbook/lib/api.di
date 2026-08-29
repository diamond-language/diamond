# The stateless, cross-origin side of the app: no session, no CSRF (a
# third-party origin consuming this endpoint has no session-embedded
# CSRF token to send, and doesn't need one for a read-only GET) --
# just Cors, so a trusted partner origin's own JS can read the response.

def api_status(request, context)
  [200, {"Content-Type": "application/json"}, JSON.stringify({"ok": true})]
end
