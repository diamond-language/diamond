# Runtime helpers shared by every compiled template: HTML-escaping and a
# thin Rack response adapter. `Drb.escape_html` is the canonical
# implementation -- `lib/drb/compiler.di` inlines an equivalent, separately
# hand-written copy into every generated `.di` file (see that file's
# `escape_helper_lines`) so a compiled template has zero runtime dependency
# on this package. Keep the two in behavioral sync; `test.sh` checks this
# directly by comparing this function's output against a generated
# template's own escaping for the same inputs.
module Drb
  module_function

  def escape_html(value)
    s = "#{value}"
    sb = StringBuilder.new()
    i = 0
    while i < s.length()
      ch = s[i]
      if ch == "&"
        sb.append("&amp;")
      elsif ch == "<"
        sb.append("&lt;")
      elsif ch == ">"
        sb.append("&gt;")
      elsif ch == "\""
        sb.append("&quot;")
      elsif ch == "'"
        sb.append("&#39;")
      else
        sb.append(ch)
      end
      i += 1
    end
    sb.to_s()
  end

  # Wraps a rendered template body into the [status, headers, body] shape
  # packages/rack (and http_serve/gremlin_serve directly) already expect --
  # packages/rack itself stays untouched and dependency-free; drb adapts to
  # its convention rather than the other way around.
  def html_response(status, body, headers = {})
    merged = headers.merge({"Content-Type": "text/html"})
    [status, merged, body]
  end
end
