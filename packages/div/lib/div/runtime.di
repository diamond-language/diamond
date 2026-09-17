# Runtime helpers shared by every compiled template: HTML-escaping and a
# thin Rack response adapter. `Div.escape_html` is the canonical
# implementation -- `lib/div/compiler.di` inlines an equivalent, separately
# hand-written copy into every generated `.di` file (see that file's
# `escape_helper_lines`) so a compiled template has zero runtime dependency
# on this package. Keep the two in behavioral sync; `test.sh` checks this
# directly by comparing this function's output against a generated
# template's own escaping for the same inputs.
module Div
  module_function

  def escape_html(value)
    s = "#{value}"
    # Fast path: the overwhelmingly common case is a string with none of
    # the 5 characters that need escaping at all (a title, username,
    # platform name, ...) -- skip the char-by-char StringBuilder loop
    # below entirely rather than paying one single-character String
    # allocation per byte for a string that ends up byte-identical to its
    # own input. `.index_of` is a native, non-allocating scan; String
    # indexing/length are byte-based here (see docs/collections.md's own
    # "byte-level, not UTF-8 grapheme-aware" note on .reverse()), so this
    # is safe for multi-byte UTF-8 content too -- none of these 5 ASCII
    # bytes can appear as part of a different codepoint's encoding.
    if s.index_of("&") == nil && s.index_of("<") == nil && s.index_of(">") == nil && s.index_of("\"") == nil && s.index_of("'") == nil
      return s
    end
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

  # `<input type="hidden">` tag, both `name` and `value` escaped. Small
  # enough to earn its keep on its own (CSRF tokens are the main use:
  # `<%== Div.hidden_field_tag("csrf_token", csrf_token) %>`), and general
  # rather than CSRF-specific since the escaping it needs has nothing to
  # do with what the field holds.
  def hidden_field_tag(name, value)
    "<input type=\"hidden\" name=\"#{Div.escape_html(name)}\" value=\"#{Div.escape_html(value)}\">"
  end

  # Wraps a rendered template body into the [status, headers, body] shape
  # packages/rack (and http_serve/gremlin_serve directly) already expect --
  # packages/rack itself stays untouched and dependency-free; div adapts to
  # its convention rather than the other way around.
  def html_response(status, body, headers = {})
    merged = headers.merge({"Content-Type": "text/html"})
    [status, merged, body]
  end
end
