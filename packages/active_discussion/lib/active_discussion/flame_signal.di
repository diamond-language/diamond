module ActiveDiscussion

# Ported from ActiveDiscussion's own FlameSignal -- a lightweight
# per-discussion report/flag (spam/abuse/flame/report, see signal.di).
# This is the one moderation primitive kept from the wider
# MaquinasStack review (see README.md): the separate case/vote
# tribunal and forum-level permission scheme found duplicating this
# elsewhere in the Ruby monorepo are not ported.
class FlameSignal < ActiveRecord::Model
  attr_accessor discussion_id, persona_handle: String, signal_type: String, flagged_by, created_at

  def initialize(attributes: Hash = {})
    super(attributes)
    @discussion_id = attributes["discussion_id"]
    @persona_handle = attributes["persona_handle"]
    @signal_type = attributes["signal_type"]
    @flagged_by = attributes["flagged_by"]
    @created_at = if attributes["created_at"] == nil then Time.now().to_i() else attributes["created_at"] end
  end

  def to_attributes() = {"discussion_id": @discussion_id, "persona_handle": @persona_handle,
    "signal_type": @signal_type, "flagged_by": @flagged_by, "created_at": @created_at}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  # Every signal against `discussion_id`, newest first.
  def self.signals_for(db, discussion_id) -> Array
    rows = FlameSignal.where({"discussion_id": discussion_id}).to_a(db)
    rows.sort_by() do |signal| -signal.created_at() end
  end

  # How many signals against `discussion_id` landed within the last
  # `window_seconds` -- used to auto-trigger a cooldown (see
  # Discussion#signal! in discussion.di).
  def self.recent_count(db, discussion_id, window_seconds) -> Int
    cutoff = Time.now().to_i() - window_seconds
    rows = FlameSignal.where({"discussion_id": discussion_id}).to_a(db)
    count = 0
    rows.each() do |signal|
      if signal.created_at() >= cutoff
        count += 1
      end
    end
    count
  end
end

end

# A genuine top-level function, not nested inside `module
# ActiveDiscussion` above -- see packages/active_auth/README.md for
# why (a top-level function referencing a namespaced class needs that
# class already declared earlier in the same file).
def build_flame_signal(row) = ActiveDiscussion::FlameSignal.new(row)
