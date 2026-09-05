# Percent-encodes `text` for safe embedding in a URL query-string value
# (application/x-www-form-urlencoded: space -> "+", everything outside
# [A-Za-z0-9-_.~] -> "%XX"). No dedicated URL-encoding builtin exists in
# Diamond (checked directly), but nothing beyond String#ord/#slice is
# actually needed to build one -- byte-by-byte, matching how a multi-
# byte UTF-8 character is *supposed* to come out percent-encoded anyway
# (one %XX per raw byte, exactly what a real browser's own
# encodeURIComponent produces), since Diamond's String is already
# documented as a raw, binary-safe byte buffer (packages/multipart's
# own comment).
#
# A bare top-level function, not `module Dials`-scoped -- matches this
# package's own `cookie_parse`/`cookie_serialize`-style precedent in
# packages/cookies (top-level, called unqualified), and lets a consuming
# app's own view templates keep interpolating this directly
# (`url_encode(query_text)`) with no qualification, exactly as skindicate
# already did before this moved here from its own lib/helpers/url.di.
def url_encode(text)
  hex_digits = "0123456789ABCDEF"
  result = ""
  index = 0
  while index < text.length()
    ch = text.slice(index, 1)
    code = ch.ord()
    is_unreserved = (code >= 65 && code <= 90) || (code >= 97 && code <= 122) ||
      (code >= 48 && code <= 57) || ch == "-" || ch == "_" || ch == "." || ch == "~"
    if is_unreserved
      result = "#{result}#{ch}"
    elsif ch == " "
      result = "#{result}+"
    else
      high = code / 16
      low = code % 16
      result = "#{result}%#{hex_digits.slice(high, 1)}#{hex_digits.slice(low, 1)}"
    end
    index += 1
  end
  result
end
