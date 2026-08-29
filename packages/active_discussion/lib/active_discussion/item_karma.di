module ActiveDiscussion

# Slashdot-style item karma -- ported from ActiveDiscussion's own
# ItemKarma. Only MIN (-1) is hidden from the default view; 0-5
# display normally. Pure value logic, no persistence of its own (the
# karma value itself lives on Discussion/Comment).
module ItemKarma
  MIN = -1
  MAX = 5
  DEFAULT_MIN = 0 # new posts never spawn in the penalty box

  def self.clamp(karma: Int) -> Int
    if karma < MIN then MIN elsif karma > MAX then MAX else karma end
  end

  def self.clamp_default(karma: Int) -> Int
    if karma < DEFAULT_MIN then DEFAULT_MIN elsif karma > MAX then MAX else karma end
  end

  def self.label_for(karma: Int) -> String
    clamped = ItemKarma.clamp(karma)
    if clamped == -1 then "you're in the penalty box"
    elsif clamped == 0 then "you exist, but barely"
    elsif clamped == 1 then "baseline trust"
    elsif clamped == 2 then "you're contributing"
    elsif clamped == 3 then "you're reliable"
    elsif clamped == 4 then "you're respected"
    else "you're a community pillar"
    end
  end

  # Snapshots an ActiveKarma::PersonaState (packages/active_karma) into
  # a starting item-karma value, frozen at post time -- ported from
  # initial_from_persona_state, but dispatches directly against
  # PersonaState's own real predicates instead of Ruby's `respond_to?`
  # duck-typing, since Diamond code calling this always has a real
  # PersonaState (or nil), never something merely PersonaState-shaped.
  # `nil` (no karma system configured) matches the Ruby original's own
  # `else` branch: full trust by default.
  def self.initial_from_persona_state(state) -> Int
    raw = if state == nil
      5
    elsif state.write_denied?() || state.blocked?() || state.shadowbanned?() || state.shadow?()
      0
    elsif state.limited?() || state.write_throttled?()
      2
    elsif state.review_required?()
      3
    else
      5
    end
    ItemKarma.clamp_default(raw)
  end

  # Visible in the default feed when karma >= threshold (default 0) --
  # only MIN is hidden by default. A moderator always sees everything;
  # an item's own author always sees their own item regardless of karma.
  def self.visible?(karma: Int, author_handle = nil, viewer_handle = nil, moderator: Bool = false, threshold: Int = 0) -> Bool
    if moderator
      return true
    end
    if viewer_handle != nil && viewer_handle == author_handle
      return true
    end
    karma >= threshold
  end

  def self.hidden?(karma: Int, author_handle = nil, viewer_handle = nil, moderator: Bool = false, threshold: Int = 0) -> Bool
    !ItemKarma.visible?(karma, author_handle, viewer_handle, moderator, threshold)
  end

  def self.penalty_box?(karma: Int) -> Bool = ItemKarma.clamp(karma) == MIN
end

end
