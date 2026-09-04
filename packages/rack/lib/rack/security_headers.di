# SecurityHeaders: a Callable[3] middleware that adds a standard set of
# defensive response headers -- the kind every app wants, framework-
# level, rather than every app_handler remembering to set them itself.
# Runs *after* forward (it needs the real response to add headers onto),
# so its own position in the chain relative to other middlewares mostly
# doesn't matter -- unlike CookieSession/Csrf (packages/cookies), this
# has no ordering requirement of its own.
#
# `.configure(options)` mirrors CookieSession's own class-variable
# pattern (see packages/cookies/README.md's own explanation) for the
# same reason: each Thread.new-spawned gremlin_serve worker gets its own
# independent DiamondVm, so non-default options need setting inside
# whatever function already runs once per worker (the `build_chain()`
# RackChain already calls per worker), not captured in a closure.
# `.call` works with zero configuration too -- every option below has a
# safe default.
#
# Options (all optional):
# - "frame_options": X-Frame-Options value, default "SAMEORIGIN" --
#   pass `false` to omit the header entirely (an app that legitimately
#   needs to be framed).
# - "content_type_options": set `false` to omit X-Content-Type-Options
#   (default: included, as "nosniff" -- there's no legitimate reason to
#   want MIME-sniffing on, so this has no other configurable value).
# - "referrer_policy": Referrer-Policy value, default
#   "strict-origin-when-cross-origin".
# - "content_security_policy": Content-Security-Policy value -- omitted
#   by default (nil), since a safe default policy is too app-specific to
#   guess (it depends on where an app's own scripts/styles/images
#   actually come from); set explicitly per app.
# - "hsts": Strict-Transport-Security -- omitted by default (nil),
#   deliberately not turned on automatically: HSTS instructs browsers to
#   refuse plain HTTP to this host for the given duration, which is
#   actively harmful to set on a service that isn't genuinely served
#   over TLS yet (a local http_serve dev setup, say). Pass `true` for a
#   sane default ("max-age=31536000; includeSubDomains"), or a literal
#   value to fully control it.
#
# X-XSS-Protection is deliberately not included at all -- current OWASP
# guidance is to omit it (or send "0"): the legacy browser XSS filters it
# controlled are removed from every modern browser, and enabling it on
# the browsers that still had it introduced real cross-site leak
# vulnerabilities of its own.
class SecurityHeaders
  def self.configure(options = {})
    @@options = options
  end

  def self.call(request, context, forward)
    response = forward(request, context)
    # `nil` means a handler already fully took over the connection
    # itself (see packages/gremlin's own "Escape hatch: taking over the
    # raw connection" doc comment -- a WebSocket upgrade is the
    # motivating case) -- there's no [status, headers, body] to add
    # security headers onto, so this passes it straight through.
    if response == nil
      return nil
    end
    [status, headers, body] = response
    options = if @@options == nil then {} else @@options end
    security = {}

    frame_options = options["frame_options"]
    unless frame_options == false
      security["X-Frame-Options"] = if frame_options == nil then "SAMEORIGIN" else frame_options end
    end

    unless options["content_type_options"] == false
      security["X-Content-Type-Options"] = "nosniff"
    end

    referrer_policy = options["referrer_policy"]
    security["Referrer-Policy"] = if referrer_policy == nil then "strict-origin-when-cross-origin" else referrer_policy end

    csp = options["content_security_policy"]
    unless csp == nil
      security["Content-Security-Policy"] = csp
    end

    hsts = options["hsts"]
    unless hsts == nil || hsts == false
      security["Strict-Transport-Security"] = if hsts == true then "max-age=31536000; includeSubDomains" else hsts end
    end

    [status, headers.merge(security), body]
  end
end
