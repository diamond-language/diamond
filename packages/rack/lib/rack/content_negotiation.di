# ContentNegotiation: picks which representation of a response to send
# back based on what the client actually asked for, instead of an app
# hard-coding a single Content-Type per route (every existing example
# in this package's own README does exactly that). Not a middleware --
# called directly from a handler/controller, the way Dials::Response's
# helpers already are, since it needs to build the *chosen*
# representation's body itself (a Callable, not a plain value -- see
# #respond_to below).
#
# Two independent ways a client can ask for a format, tried in this
# order:
# - A trailing dot-extension already sitting in the request path
#   (".json", ".html", ...) -- checked directly against `path`, already
#   on the raw request Hash both http_serve and gremlin_serve hand
#   every middleware, with zero dependency on how (or whether) an app's
#   own router parses a `:format` segment. Pass `strip_path_format:
#   false` for a route that never carries one, so a resource whose real
#   identifier happens to contain a dot (a filename, say) isn't
#   misread as a format suffix.
# - The `Accept` header, real content negotiation (comma-separated
#   media ranges, `;q=` weights, `*/*`/`type/*` wildcards) -- not just
#   "does the header contain this substring somewhere."
#
# Falls back to `default_format` when neither names an available
# representation. This module has no dependency on Dials or Div --
# same "protocol-generic" stance as Dials::Response itself (see that
# class's own comment) -- so it works from the raw request/response
# convention alone, usable from any Diamond web app built on this
# package. Top-level, not namespaced under a `module Rack` -- matches
# every other class in this package (StaticFiles, Cors, RateLimit).
class ContentNegotiation
  def self.mime_type_for(format)
    case format
    when "html" then "text/html"
    when "json" then "application/json"
    when "text", "txt" then "text/plain"
    when "xml" then "application/xml"
    else "application/octet-stream"
    end
  end

  # request["path"]'s own trailing ".<ext>", downcased, or nil if there
  # isn't one -- also nil for a dot with nothing on one side of it
  # ("/.json" with no resource, or a path ending in a bare "."), so
  # neither false-positives as a format suffix.
  def self.format_from_path(path)
    last_segment = path.split("/").last()
    if last_segment == nil || last_segment == "" then return nil end
    reversed = last_segment.reverse()
    dot_from_end = reversed.index_of(".")
    if dot_from_end == nil then return nil end
    extension = last_segment.slice(last_segment.length() - dot_from_end, dot_from_end)
    stem = last_segment.slice(0, last_segment.length() - dot_from_end - 1)
    if extension == "" || stem == "" then return nil end
    extension.downcase()
  end

  # media_type is already-trimmed, e.g. "text/html", "application/json",
  # "*/*", "text/*". Matches an available format if its own mime type
  # equals the media range exactly, or the range is a wildcard covering
  # it (checked against the mime type's own "<type>/" prefix, not just
  # any leading substring, so "application/*" can't accidentally match
  # "text/xml" the way a plain start_with? on the raw string might).
  def self.format_for_media_range(media_type, available_formats)
    available_formats.find() do |format|
      mime = ContentNegotiation.mime_type_for(format)
      if media_type == mime || media_type == "*/*"
        true
      elsif media_type.end_with?("/*")
        wanted_type = media_type.slice(0, media_type.length() - 1)
        mime.start_with?(wanted_type)
      else
        false
      end
    end
  end

  # Parses an Accept header into the single best-matching available
  # format by descending q-value (default 1.0 when a range carries no
  # `;q=`). Ties keep header order -- this walks the list once, tracking
  # the best match directly, rather than sorting.
  def self.negotiate(accept_header, available_formats, default_format)
    if accept_header == nil || accept_header == "" then return default_format end
    ranges = accept_header.split(",")
    best_format = nil
    best_q = -1.0
    ranges.each() do |range|
      parts = range.strip().split(";")
      media_type = parts[0].strip()
      q = 1.0
      parts.drop(1).each() do |raw_param|
        param = raw_param.strip()
        if param.start_with?("q=")
          q = param.slice(2, param.length() - 2).to_f()
        end
      end
      format = ContentNegotiation.format_for_media_range(media_type, available_formats)
      if format != nil && q > best_q
        best_format = format
        best_q = q
      end
    end
    if best_format == nil then default_format else best_format end
  end

  # `representations`: Hash of format name -> zero-arg Callable
  # producing that representation's response body -- a Callable, not a
  # plain value, so only the chosen representation is ever actually
  # built (rendering a full HTML page *and* a JSON dump of the same
  # data on every request, just to throw one away, would be wasteful
  # the moment either is non-trivial). Returns the full
  # [status, headers, body] tuple this package's own middleware
  # contract already expects, Content-Type set from the chosen format,
  # `extra_headers` merged in on top. 406 (with a plain-text body, not
  # one of the representations -- none were an acceptable match, so
  # there's nothing safe to build) if the client named an actual format
  # (path suffix or Accept header) this call has no representation for
  # at all; an absent/empty Accept header already falls back to
  # `default_format` inside #negotiate rather than reaching here.
  def self.respond_to(request, status: Int, representations: Hash, default_format: String,
                       extra_headers: Hash = {}, strip_path_format: Bool = true)
    available = representations.keys()
    format = if strip_path_format then ContentNegotiation.format_from_path(request["path"]) else nil end
    if format == nil || !available.include?(format)
      format = ContentNegotiation.negotiate(request["headers"]["accept"], available, default_format)
    end
    builder = representations[format]
    if builder == nil
      return [406, {"Content-Type": "text/plain"}, "not acceptable"]
    end
    headers = {"Content-Type": ContentNegotiation.mime_type_for(format)}.merge(extra_headers)
    [status, headers, builder()]
  end
end
