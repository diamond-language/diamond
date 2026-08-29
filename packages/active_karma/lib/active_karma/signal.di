module ActiveKarma

# The fixed vocabulary of system-authored trust signals -- ported from
# MaquinasStack's ActiveKarma gem's own Signal module. Every value is
# namespaced "karma.xxx" so a persisted event log (added by whatever
# app wires this up -- see README.md) can share a table with other
# event kinds without collision.
module Signal
  SPAM_DETECTED = "karma.spam_detected"
  ABUSE_DETECTED = "karma.abuse_detected"
  COMPROMISED = "karma.compromised"
  RATE_LIMITED = "karma.rate_limited"
  ANOMALY_DETECTED = "karma.anomaly_detected"
  REVIEW_STARTED = "karma.review_started"
  REVIEW_CLEARED = "karma.review_cleared"
  MANUALLY_LIMITED = "karma.manually_limited"
  MANUALLY_BLOCKED = "karma.manually_blocked"
  MANUALLY_SHADOWBANNED = "karma.manually_shadowbanned"
  MANUALLY_CLEARED = "karma.manually_cleared"
  VERIFICATION_REQUIRED = "karma.verification_required"
  VERIFICATION_PROVIDED = "karma.verification_provided"

  def self.all()
    [SPAM_DETECTED, ABUSE_DETECTED, COMPROMISED,
     RATE_LIMITED, ANOMALY_DETECTED, REVIEW_STARTED,
     REVIEW_CLEARED, MANUALLY_LIMITED, MANUALLY_BLOCKED,
     MANUALLY_SHADOWBANNED, MANUALLY_CLEARED,
     VERIFICATION_REQUIRED, VERIFICATION_PROVIDED]
  end

  def self.namespaced(flag: String) -> String
    if flag.length() >= 6 && flag.slice(0, 6) == "karma."
      flag
    else
      "karma.#{flag}"
    end
  end

  def self.valid?(flag: String) -> Bool
    Signal.all().include?(Signal.namespaced(flag))
  end
end

end
