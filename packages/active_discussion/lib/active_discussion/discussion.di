module ActiveDiscussion

class DiscussionLocked < StandardError
end
class CoolingDown < StandardError
end
class DepthExceeded < StandardError
end
class KarmaInsufficient < StandardError
end

# Ported from ActiveDiscussion's own Discussion -- the thread root a
# discussion's comments attach to. `recording_id` is an opaque id for
# whatever this discussion is attached to (e.g. a Skin's own id in
# applications/skindicate.dia) -- named to match the Ruby original
# rather than renamed to something Diamond-specific, even though it no
# longer refers to a real ActiveStenographer recording (see
# packages/active_karma/README.md for why that layer isn't ported).
class Discussion < ActiveRecord::Model
  attr_accessor recording_id: String, persona_handle, title, body, karma, max_depth, karma_floor, locked, locked_at, locked_reason, cooldown_until

  def initialize(attributes: Hash = {})
    super(attributes)
    @recording_id = attributes["recording_id"]
    @persona_handle = attributes["persona_handle"]
    @title = attributes["title"]
    @body = attributes["body"]
    @karma = if attributes["karma"] == nil then 0 else attributes["karma"] end
    @max_depth = attributes["max_depth"]
    @karma_floor = attributes["karma_floor"]
    @locked = if attributes["locked"] == nil then false else attributes["locked"] end
    @locked_at = attributes["locked_at"]
    @locked_reason = attributes["locked_reason"]
    @cooldown_until = attributes["cooldown_until"]
  end

  def to_attributes() = {"recording_id": @recording_id, "persona_handle": @persona_handle,
    "title": @title, "body": @body, "karma": @karma, "max_depth": @max_depth,
    "karma_floor": @karma_floor, "locked": @locked, "locked_at": @locked_at,
    "locked_reason": @locked_reason, "cooldown_until": @cooldown_until}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def self.open!(db, recording_id, persona_handle = nil, title = nil, body = nil)
    discussion = Discussion.new({"recording_id": recording_id, "persona_handle": persona_handle,
      "title": title, "body": body, "karma": initial_karma_for(persona_handle)})
    discussion.save(db)
    discussion
  end

  def self.find_or_open(db, recording_id, persona_handle = nil, title = nil, body = nil)
    existing = Discussion.where({"recording_id": recording_id}).first(db)
    if existing != nil
      return existing
    end
    Discussion.open!(db, recording_id, persona_handle, title, body)
  end

  def item_visible?(viewer_handle = nil, moderator: Bool = false) -> Bool
    ItemKarma.visible?(@karma, @persona_handle, viewer_handle, moderator, Configuration.hide_karma_threshold())
  end

  def has_head?() -> Bool = (@title != nil && @title != "") || (@body != nil && @body != "")

  def comments(db) -> Array
    rows = Comment.where({"discussion_id": self.id()}).to_a(db)
    rows.sort_by() do |comment| comment.created_at() * 1000000000 + comment.id() end
  end

  def top_level_comments(db) -> Array
    rows = Comment.where({"discussion_id": self.id(), "parent_id": nil}).to_a(db)
    rows.sort_by() do |comment| comment.created_at() * 1000000000 + comment.id() end
  end

  def comment_count(db) -> Int = Comment.where({"discussion_id": self.id()}).to_a(db).length()

  def locked?() -> Bool = @locked == true
  def cooling_down?() -> Bool = @cooldown_until != nil && @cooldown_until > Time.now().to_i()
  def suppressed?() -> Bool = self.locked?() || self.cooling_down?()
  def open?() -> Bool = !self.suppressed?()

  def depth_ok?(depth) -> Bool = @max_depth == nil || depth <= @max_depth

  def karma_ok?(karma_state) -> Bool
    if @karma_floor == nil
      return true
    end
    checker = Configuration.karma_checker()
    if checker == nil
      return true
    end
    checker(karma_state, @karma_floor)
  end

  def admits_comment?(depth, karma_state = nil) -> Bool
    !self.locked?() && !self.cooling_down?() && self.depth_ok?(depth) && self.karma_ok?(karma_state)
  end

  # The main entry point for posting into this discussion -- ported
  # from ActiveDiscussion::Repository#comment!. Raises a typed error
  # (DiscussionLocked/CoolingDown/DepthExceeded/KarmaInsufficient) when
  # the discussion won't admit the comment, matching the Ruby original.
  def create_comment!(db, persona_handle, body, parent_id = nil, karma_state = nil, created_at = nil)
    trimmed = body.strip()
    errors = build_comment_body_validator()({"persona_handle": persona_handle, "body": trimmed}, nil)
    unless errors.empty?()
      raise ActiveRecord::ValidationError.new(errors)
    end

    parent = nil
    unless parent_id == nil
      parent = Comment.where({"id": parent_id, "discussion_id": self.id()}).first(db)
      if parent == nil
        raise ArgumentError.new("parent comment not found in discussion: #{parent_id}")
      end
    end
    depth = if parent == nil then 0 else parent.depth() + 1 end

    if self.locked?()
      raise DiscussionLocked.new("discussion is locked")
    end
    if self.cooling_down?()
      raise CoolingDown.new("discussion is cooling down")
    end
    unless self.depth_ok?(depth)
      raise DepthExceeded.new("comment depth #{depth} exceeds this discussion's limit")
    end
    unless self.karma_ok?(karma_state)
      raise KarmaInsufficient.new("karma does not meet this discussion's floor")
    end

    now = if created_at == nil then Time.now().to_i() else created_at end
    parent_comment_id = if parent == nil then nil else parent.id() end
    comment = Comment.new({"discussion_id": self.id(), "parent_id": parent_comment_id,
      "persona_handle": persona_handle, "body": trimmed, "karma": initial_karma_for(persona_handle),
      "depth": depth, "created_at": now, "updated_at": now})
    comment.save(db)
    comment
  end

  def lock!(db, reason = nil)
    self.locked = true
    self.locked_at = Time.now().to_i()
    self.locked_reason = reason
    self.save(db)
  end

  def unlock!(db)
    self.locked = false
    self.locked_at = nil
    self.locked_reason = nil
    self.cooldown_until = nil
    self.save(db)
  end

  def cool_down!(db, duration = nil)
    secs = if duration == nil then Configuration.cooldown_duration() else duration end
    self.cooldown_until = Time.now().to_i() + secs
    self.save(db)
  end

  def configure_limits!(db, max_depth = :unchanged, karma_floor = :unchanged)
    unless max_depth == :unchanged
      self.max_depth = max_depth
    end
    unless karma_floor == :unchanged
      self.karma_floor = karma_floor
    end
    self.save(db)
  end

  # Records a flame signal against a persona in this discussion.
  # Auto-triggers a cooldown if the recent signal count within the
  # configured window crosses the configured threshold.
  def signal!(db, persona_handle, type: String, flagged_by = nil)
    unless Signal.valid?(type)
      raise ArgumentError.new("unknown signal type: #{type}")
    end
    FlameSignal.create(db, {"discussion_id": self.id(), "persona_handle": persona_handle,
      "signal_type": type, "flagged_by": flagged_by, "created_at": Time.now().to_i()})
    recent = FlameSignal.recent_count(db, self.id(), Configuration.flame_window())
    if recent >= Configuration.flame_threshold()
      self.cool_down!(db)
    end
  end

  def signals_for(db) -> Array = FlameSignal.signals_for(db, self.id())

  def vote_karma!(db, voter_handle, value) = apply_karma_vote!(db, "discussion", self, voter_handle, value)
  def clear_karma_vote!(db, voter_handle) = clear_karma_vote_for!(db, "discussion", self, voter_handle)
end

end

def build_discussion(row) = ActiveDiscussion::Discussion.new(row)
