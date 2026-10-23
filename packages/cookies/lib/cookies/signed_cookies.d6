# Tamper-evident, not confidential -- for cases like a CSRF token or a
# plain user id where readability doesn't matter. base64url(value) + "."
# + its HMAC-SHA256 hex signature; .verify uses HMAC.verify's constant-
# time comparison rather than plain `==`, matching how BCrypt.verify
# already avoids a timing side-channel on its own comparison.
class SignedCookies
  def self.sign(value: String, secret: String) -> String
    "#{base64url_encode(value)}.#{HMAC.sha256(secret, value)}"
  end

  def self.verify(cookie_value: String, secret: String)
    dot = cookie_value.index_of(".")
    if dot == nil
      return nil
    end
    encoded = cookie_value.slice(0, dot)
    signature = cookie_value.slice(dot + 1, cookie_value.length())
    value = base64url_decode(encoded)
    if value == nil
      return nil
    end
    if HMAC.verify(value, secret, signature)
      value
    else
      nil
    end
  end
end
