# Appends `value` onto whatever `existing` already holds for a
# response's Set-Cookie entry -- nil (nothing set yet), a single
# String (one cookie already set), or an Array (several already set) --
# returning the Array to store back. Kept as its own top-level function
# rather than nested inside CookieSession.call: a nested `def` can only
# close over a handful of outer locals (DIAMOND_MAX_BOUND_VALUES, see
# src/vm.c), and .call already has too many in scope by this point.
def cookie_session_merge_header(existing, value)
  merged = []
  unless existing == nil
    case existing
    when [*lines]
      def collect_line(line)
        merged.push(line)
      end
      lines.each(collect_line)
    else
      merged.push(existing)
    end
  end
  merged.push(value)
  merged
end

# The actual Rack cookie-session middleware: reads a named cookie,
# decrypts+JSON-parses it into request["session"] (a plain mutable Hash,
# {} if missing/tampered/wrong secret) before calling `forward`; after
# `forward` returns, re-serializes the (possibly mutated) session Hash
# back into a Set-Cookie response header.
#
# Must be configured once per VM before use -- `.configure` sets class
# variables, and (see packages/rack's own README on RackChain) each
# Thread.new-spawned gremlin_serve worker already gets its own fully
# independent DiamondVm, so `.configure` needs calling inside whatever
# function already runs once per worker (the same `build_chain()`-style
# function RackChain's own pattern already calls per worker), not just
# once at the top of the script before threads are spawned:
#
#   def build_chain()
#     CookieSession.configure(secret: ENV["SESSION_SECRET"])
#     rack_compose([CookieSession], app_handler)
#   end
#
#   def rack_app(request, context)
#     rack_run_chain(RackChain.get(build_chain), 0, request, context)
#   end
class CookieSession
  def self.configure(secret: String, cookie_name: String = "_session")
    @@secret = secret
    @@cookie_name = cookie_name
  end

  def self.call(request, context, forward)
    cookie_name = @@cookie_name
    cookies = cookie_parse(request["headers"]["cookie"])
    raw = cookies[cookie_name]
    session = {}
    unless raw == nil
      decrypted = EncryptedCookies.decrypt(raw, @@secret)
      unless decrypted == nil
        begin
          session = JSON.parse(decrypted)
        rescue error: JSONError
          session = {}
        end
      end
    end
    # Flash rotation (see cookies/flash.di's own comment for the full
    # design): whatever a *previous* request wrote into "_flash_next"
    # becomes readable as "_flash" on this one, and this request gets a
    # fresh, empty "_flash_next" of its own to write into. Done here,
    # once per request before `forward` runs, rather than inside Flash
    # itself -- Flash has no per-request hook of its own to do this from,
    # and every request already passes through CookieSession exactly
    # once by construction.
    next_flash = session["_flash_next"]
    session["_flash"] = if next_flash == nil then {} else next_flash end
    session["_flash_next"] = {}
    request["session"] = session
    response = forward(request, context)
    [status, headers, body] = response
    encoded = EncryptedCookies.encrypt(JSON.stringify(session), @@secret)
    set_cookie_value = cookie_serialize(cookie_name, encoded, {})
    merged = cookie_session_merge_header(headers["Set-Cookie"], set_cookie_value)
    [status, headers.merge({"Set-Cookie": merged}), body]
  end
end
