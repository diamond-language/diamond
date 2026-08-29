# StaticFiles: a Callable[3] middleware serving plain files straight off
# disk -- the framework's whole "asset layer": no bundling, no
# fingerprinting/cache-busting, no build step. Drop a .js/.css/image/
# font file under a directory, it's reachable at the matching URL path.
# Deliberately just this -- see README.md's own "What's deliberately out
# of scope" for what a real asset pipeline would add on top, and add it
# only once a concrete app actually needs it.
#
# `.configure(options)` mirrors the other middlewares in this package --
# a class-variable write, so on `threads: N` it needs calling inside the
# same `build_chain()`-style function that already runs once per
# `Thread.new`-spawned worker:
# - "root": the directory to serve files from (required).
# - "prefix": a URL prefix required before the on-disk path, e.g.
#   `"/assets"` so `GET /assets/app.css` serves `<root>/app.css` --
#   defaults to `""` (no prefix), so `GET /app.css` serves
#   `<root>/app.css` directly -- the plain "public directory" shape.
#
# Only `GET` is handled; every other method falls through to `forward`
# unconditionally. A `GET` is served from disk when the path (after
# stripping `prefix`, if any) resolves to a real, readable file --
# checked by actually trying to open and read it (`File.open`/`.read`,
# both of which raise a rescuable `IOError` on failure -- a missing
# file, a directory, a permissions error, all indistinguishable from
# here and all treated the same way: fall through to `forward` instead
# of erroring, so an app's own route at the same path still gets a
# chance to handle the request). No directory listing, no index.html
# resolution for a directory path -- both deliberately out of scope.
#
# Path traversal is blocked by rejecting any ".." path segment outright
# before ever touching the filesystem -- sufficient because
# packages/http's own request parser never percent-decodes the path (see
# that package's own server.di), so a literal ".." is the only way a
# request could ever name one; there is no encoded form to also guard
# against here.
class StaticFiles
  def self.configure(options)
    @@root = options["root"]
    prefix = options["prefix"]
    @@prefix = if prefix == nil then "" else prefix end
  end

  # nil means "don't serve this" (wrong prefix, or a ".." segment
  # anywhere) -- the caller falls through to `forward` either way, the
  # same as a file that isn't found, so this doesn't need to distinguish
  # "blocked" from "not ours to serve."
  def self.safe_relative_path(path)
    relative = path
    if @@prefix != ""
      if !relative.start_with?(@@prefix)
        return nil
      end
      relative = relative.slice(@@prefix.length(), relative.length() - @@prefix.length())
    end
    if relative.start_with?("/")
      relative = relative.slice(1, relative.length() - 1)
    end
    if relative == ""
      return nil
    end
    segments = relative.split("/")
    index = 0
    while index < segments.length()
      if segments[index] == ".."
        return nil
      end
      index = index + 1
    end
    relative
  end

  # The extension is whatever follows the *last* "." (String#index_of
  # only ever finds the first occurrence, so this reverses the string to
  # find the last dot from the end instead, then converts that back into
  # a position from the start) -- "app.min.js" resolves to "js", not
  # "min.js". An unrecognized or missing extension serves as
  # "application/octet-stream" -- a safe, generic default (paired with
  # SecurityHeaders' own X-Content-Type-Options: nosniff default,
  # browsers won't guess a more specific type and render it unexpectedly).
  def self.content_type_for(path)
    reversed = path.reverse()
    dot_from_end = reversed.index_of(".")
    if dot_from_end == nil
      return "application/octet-stream"
    end
    extension = path.slice(path.length() - dot_from_end, dot_from_end).downcase()
    case extension
    when "html", "htm" then "text/html"
    when "css" then "text/css"
    when "js", "mjs" then "text/javascript"
    when "json" then "application/json"
    when "svg" then "image/svg+xml"
    when "png" then "image/png"
    when "jpg", "jpeg" then "image/jpeg"
    when "gif" then "image/gif"
    when "ico" then "image/x-icon"
    when "woff" then "font/woff"
    when "woff2" then "font/woff2"
    when "txt" then "text/plain"
    when "pdf" then "application/pdf"
    else "application/octet-stream"
    end
  end

  def self.call(request, context, forward)
    if request["method"] != "GET"
      return forward(request, context)
    end
    relative = StaticFiles.safe_relative_path(request["path"])
    if relative == nil
      return forward(request, context)
    end
    full_path = "#{@@root}/#{relative}"
    body = nil
    begin
      file = File.open(full_path, "r")
      body = file.read()
      file.close()
    rescue error: IOError
      return forward(request, context)
    end
    [200, {"Content-Type": StaticFiles.content_type_for(relative)}, body]
  end
end
