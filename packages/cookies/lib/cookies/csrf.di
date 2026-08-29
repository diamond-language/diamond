# CSRF protection via the synchronizer-token pattern, built on
# CookieSession's own request["session"] -- must run *after*
# CookieSession in the middleware chain (rack_compose([CookieSession.call,
# Csrf.call], app_handler)), the same ordering requirement any session-
# dependent middleware has. `.token(request)` lazily mints one 256-bit
# token per session (SecureRandom.hex(32), matching docs/syntax.md's own
# "session/remember-me/password-reset tokens" use of that call) and
# stores it in the session, so it survives across requests the same way
# the session itself does -- call it from your own template/handler code
# to get the value to embed in a form (as a hidden field) or hand to
# client-side JS (to echo back as the X-CSRF-Token header).
#
# `.call` ensures a token exists on every request (so the very first
# GET that renders a form already has one to embed), skips the check
# entirely for the three methods that must never carry a state-changing
# side effect (GET/HEAD/OPTIONS -- the same "safe methods" list CSRF
# protection everywhere draws this line at), and otherwise requires the
# request's own X-CSRF-Token header to match the session's token via
# constant_time_equal, rejecting with 403 if it's missing or doesn't
# match. Checking a request header rather than parsing a form body is
# deliberate: this codebase has no built-in form-body parser to hook
# into (see packages/http's own README), and a header is also what an
# XHR/fetch-based client sends most naturally -- a traditional HTML
# form submission needs its own page to read Csrf.token(request) into a
# hidden field and a tiny client-side script to copy it into the header
# instead, which is out of scope for this middleware itself.
class Csrf
  def self.token(request)
    token = request["session"]["csrf_token"]
    if token == nil
      token = SecureRandom.hex(32)
      request["session"]["csrf_token"] = token
    end
    token
  end

  def self.valid?(request, submitted)
    token = request["session"]["csrf_token"]
    if token == nil || submitted == nil
      false
    else
      constant_time_equal(token, submitted)
    end
  end

  def self.call(request, context, forward)
    Csrf.token(request)
    if ["GET", "HEAD", "OPTIONS"].include?(request["method"])
      return forward(request, context)
    end
    if Csrf.valid?(request, request["headers"]["x-csrf-token"])
      forward(request, context)
    else
      [403, {"Content-Type": "text/plain"}, "invalid or missing CSRF token"]
    end
  end
end
