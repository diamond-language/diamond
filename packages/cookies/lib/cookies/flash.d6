# Flash messages -- a value that survives exactly one redirect (Rails'
# `flash[:notice]`), built on CookieSession's own request["session"]
# rather than a new mechanism. The whole feature is two session
# sub-keys plus a one-request rotation:
#
#   - "_flash": readable *this* request -- whatever a *previous*
#     request wrote via Flash.set.
#   - "_flash_next": written *this* request via Flash.set, becomes
#     "_flash" (and only "_flash") on the *next* request.
#
# CookieSession.call does the actual rotation (moving "_flash_next"
# into "_flash", then resetting "_flash_next" to {}) once per request,
# before `forward` runs -- see its own comment. Flash itself only ever
# reads "_flash" and writes "_flash_next", so a message set and read
# back within the *same* request (no redirect in between) correctly
# still reads back nil, matching Rails' own `flash[:notice] = "x"` (not
# `flash.now`) behavior.
class Flash
  # nil if nothing was set for `key` on the previous request.
  def self.get(request, key)
    request["session"]["_flash"][key]
  end

  # Every message set on the *previous* request, as a plain Hash --
  # handy for a layout that wants to render "whatever's there" without
  # knowing key names ahead of time (`for key in Flash.all(request).keys()`).
  def self.all(request)
    request["session"]["_flash"]
  end

  # Stashes `value` under `key` for the *next* request only. Typically
  # called right before a redirect (`Flash.set(request, "notice",
  # "Skin created"); Dials::Response.redirect(...)`), the same place
  # Rails' `redirect_to path, notice: "..."` shorthand would sit.
  def self.set(request, key, value)
    request["session"]["_flash_next"][key] = value
  end
end
