# Folds one `event_type`, applied oldest-first, into the state-transition
# table below -- ported directly from ActiveKarma's own Projector, same
# transitions, same precedence (e.g. RATE_LIMITED/ANOMALY_DETECTED never
# *downgrade* an existing "denied" write permission back to "throttled").
# A genuine top-level function, not nested inside `module ActiveKarma`
# below: a plain `def` (no `self.`) declared directly inside a bare
# `module ... end` block turns out not to be callable at all -- not by
# its bare name, not as `ActiveKarma.apply_karma_signal(...)` -- only
# `def self.x` and nested classes work there (confirmed directly; a new
# constraint worth remembering). Must stay defined before
# ActiveKarma::Projector.project below, which calls it -- top-level
# function calls resolve source order only, not forward (see
# packages/arel/lib/arel.di's own comment on the identical rule there).
def apply_karma_signal(event_type, state)
  if event_type == ActiveKarma::Signal::SPAM_DETECTED
    state["trust_level"] = :limited
    state["write_permission"] = :throttled
  elsif event_type == ActiveKarma::Signal::ABUSE_DETECTED
    state["trust_level"] = :blocked
    state["write_permission"] = :denied
    state["visibility"] = :shadow
  elsif event_type == ActiveKarma::Signal::COMPROMISED
    unless state["locks"].include?(:compromised)
      state["locks"].push(:compromised)
    end
    state["write_permission"] = :denied
  elsif event_type == ActiveKarma::Signal::RATE_LIMITED
    unless state["write_permission"] == :denied
      state["write_permission"] = :throttled
    end
  elsif event_type == ActiveKarma::Signal::ANOMALY_DETECTED
    state["trust_level"] = :review_required
    unless state["write_permission"] == :denied
      state["write_permission"] = :throttled
    end
  elsif event_type == ActiveKarma::Signal::REVIEW_STARTED
    state["trust_level"] = :review_required
  elsif event_type == ActiveKarma::Signal::REVIEW_CLEARED || event_type == ActiveKarma::Signal::MANUALLY_CLEARED
    state["trust_level"] = :normal
    state["write_permission"] = :allowed
    state["visibility"] = :normal
    state["locks"] = []
  elsif event_type == ActiveKarma::Signal::MANUALLY_LIMITED
    state["trust_level"] = :limited
    state["write_permission"] = :throttled
  elsif event_type == ActiveKarma::Signal::MANUALLY_BLOCKED
    state["trust_level"] = :blocked
    state["write_permission"] = :denied
  elsif event_type == ActiveKarma::Signal::MANUALLY_SHADOWBANNED
    state["trust_level"] = :shadowbanned
    state["visibility"] = :shadow
  elsif event_type == ActiveKarma::Signal::VERIFICATION_REQUIRED
    unless state["locks"].include?(:verification_required)
      state["locks"].push(:verification_required)
    end
    state["write_permission"] = :denied
  elsif event_type == ActiveKarma::Signal::VERIFICATION_PROVIDED
    current_locks = state["locks"]
    remaining_locks = current_locks.reject() do |lock| lock == :verification_required end
    state["locks"] = remaining_locks
    if remaining_locks.length() == 0 && state["trust_level"] == :normal
      state["write_permission"] = :allowed
    end
  end
end

module ActiveKarma

  class Projector
    def self.project(events: Array) -> PersonaState
      state = {"trust_level": :normal, "write_permission": :allowed, "visibility": :normal, "locks": []}
      sorted = events.sort_by() do |event| event["created_at"] end
      sorted.each() do |event|
        apply_karma_signal(event["event_type"], state)
      end
      PersonaState.new(state["trust_level"], state["write_permission"], state["visibility"], state["locks"])
    end
  end

end
