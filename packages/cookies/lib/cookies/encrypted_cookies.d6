# Confidential and tamper-evident in one step, via Cipher's AES-256-GCM
# (its own authentication tag check does the tamper-detection -- no
# separate signature needed, unlike SignedCookies, and no timing
# side-channel exposed to Diamond code either, since the whole check
# happens inside OpenSSL's EVP_DecryptFinal_ex). `secret` can be any
# length -- SHA-256 of it derives the raw 32-byte AES key Cipher itself
# requires. This is SHA-256 of an arbitrary-length secret, not a real
# KDF (PBKDF2/HKDF) -- adequate for deriving one key from one long-lived
# app secret, not a substitute for one if this ever needs to derive
# multiple independent keys from the same secret.
class EncryptedCookies
  def self.key_from_secret(secret: String) -> String
    hex_decode(Digest.sha256(secret))
  end

  def self.encrypt(value: String, secret: String) -> String
    base64url_encode(Cipher.encrypt(EncryptedCookies.key_from_secret(secret), value))
  end

  def self.decrypt(cookie_value: String, secret: String)
    blob = base64url_decode(cookie_value)
    if blob == nil
      return nil
    end
    Cipher.decrypt(EncryptedCookies.key_from_secret(secret), blob)
  end
end
