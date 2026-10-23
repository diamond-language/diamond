module ActiveAuth

  # Shared shape behind Session, EmailVerificationToken, and
  # PasswordResetToken: each stores a SHA-256 fingerprint of its token
  # ("sha256:<hex>"), never the raw presentable value (see Session's
  # own comment for the security property this gets -- the DB never
  # holds a usable token, only something that can verify one). This
  # module centralizes the parts that were byte-for-byte identical
  # across all three -- the prefix, the fingerprint function, and the
  # "is this even a candidate raw token" guard `from_token` opens with
  # -- as plain module functions rather than a shared base class: each
  # of the three still differs in table name, expiry duration, and
  # (PasswordResetToken) an extra used_at predicate, so `issue`/
  # `from_token` themselves stay each class's own job.
  module TokenSupport
    def self.prefix() = "sha256:"
    def self.fingerprint(raw: String) -> String = "#{TokenSupport.prefix()}#{Digest.sha256(raw)}"

    # True for a blank raw value or one that's already a fingerprint
    # (never a legitimately presented token) -- from_token's own guard
    # condition, shared verbatim across all three token classes.
    def self.rejected_lookup?(raw) -> Bool
      raw == nil || raw == "" ||
        (raw.length() >= 7 && raw.slice(0, 7) == TokenSupport.prefix())
    end
  end

end
