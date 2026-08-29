# Cors: a Callable[3] middleware implementing Cross-Origin Resource
# Sharing. Complements SecurityHeaders/Csrf rather than overlapping with
# them -- CORS is about which *cross-origin* requests a browser is
# allowed to let client-side JS read the response of; it's a browser-
# enforced grant, not a server-side access check, so a disallowed
# origin still gets an ordinary response (this middleware never rejects
# a request for having the wrong Origin) -- it just doesn't grant the
# `Access-Control-Allow-*` headers a browser needs to expose that
# response to cross-origin JS. A same-origin request (no Origin header
# at all) is untouched either way; CORS headers are only ever relevant
# to cross-origin requests in the first place.
#
# `.configure(options)` mirrors the other middlewares in this package --
# a class-variable write, so on `threads: N` it needs calling inside the
# same `build_chain()`-style function that already runs once per
# `Thread.new`-spawned worker:
# - "origins": `"*"` (default) allows any origin, or an `Array` of exact
#   origin strings to allow (an unlisted Origin gets no CORS headers,
#   not an error). No pattern/regex matching -- an explicit list or
#   wildcard only, matching this package's existing "no guessing" stance
#   (RateLimit's own required "key" option, for the same reason).
# - "methods": `Array` of methods to advertise in a preflight's own
#   `Access-Control-Allow-Methods` -- default `["GET", "POST", "PUT",
#   "PATCH", "DELETE", "OPTIONS"]`.
# - "credentials": `true` to send `Access-Control-Allow-Credentials:
#   true` (needed for a cross-origin request to include cookies/
#   Authorization) -- per the CORS spec, this also forces the allowed
#   origin to always be reflected back exactly rather than `"*"`, since
#   browsers reject the wildcard combined with credentials outright.
# - "max_age": seconds a browser may cache a preflight result for
#   (`Access-Control-Max-Age`) -- omitted (browser default, typically no
#   caching) unless given.
#
# A **preflight** request (`OPTIONS` carrying its own
# `Access-Control-Request-Method` header -- the browser-generated probe
# before a "non-simple" cross-origin request, not an ordinary app-level
# `OPTIONS` handler) is answered directly with `204` and never reaches
# `forward` -- `Access-Control-Allow-Headers` echoes back whatever the
# browser's own `Access-Control-Request-Headers` asked for, rather than
# a separately configured allow-list, since that's already exactly the
# request-defined answer to "which headers does this request need."
# An ordinary request calls `forward` and adds
# `Access-Control-Allow-Origin`/`-Credentials` onto whatever it returns.
class Cors
  def self.configure(options = {})
    @@origins = options["origins"]
    @@methods = options["methods"]
    @@credentials = options["credentials"]
    @@max_age = options["max_age"]
  end

  def self.allowed_origin(origin)
    origins = if @@origins == nil then "*" else @@origins end
    if origin == nil
      nil
    elsif origins == "*"
      if @@credentials == true then origin else "*" end
    elsif origins.include?(origin)
      origin
    else
      nil
    end
  end

  def self.preflight_response(request, allowed)
    headers = {}
    unless allowed == nil
      headers["Access-Control-Allow-Origin"] = allowed
      if @@credentials == true
        headers["Access-Control-Allow-Credentials"] = "true"
      end
      methods = if @@methods == nil then ["GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"] else @@methods end
      headers["Access-Control-Allow-Methods"] = methods.join(", ")
      requested_headers = request["headers"]["access-control-request-headers"]
      unless requested_headers == nil
        headers["Access-Control-Allow-Headers"] = requested_headers
      end
      unless @@max_age == nil
        headers["Access-Control-Max-Age"] = "#{@@max_age}"
      end
    end
    [204, headers, ""]
  end

  def self.call(request, context, forward)
    allowed = Cors.allowed_origin(request["headers"]["origin"])
    is_preflight = request["method"] == "OPTIONS" &&
      request["headers"]["access-control-request-method"] != nil
    if is_preflight
      return Cors.preflight_response(request, allowed)
    end
    response = forward(request, context)
    [status, headers, body] = response
    cors_headers = {}
    unless allowed == nil
      cors_headers["Access-Control-Allow-Origin"] = allowed
      if @@credentials == true
        cors_headers["Access-Control-Allow-Credentials"] = "true"
      end
    end
    [status, headers.merge(cors_headers), body]
  end
end
