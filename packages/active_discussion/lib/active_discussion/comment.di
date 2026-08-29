# A genuine top-level function, not nested inside `module
# ActiveDiscussion` below -- see packages/active_auth/README.md for
# why. References ActiveDiscussion::ItemKarma/Configuration, both in
# already-`require`d sibling files by the time active_discussion.di
# loads this one -- safe regardless of position for a *cross-file*
# reference (confirmed directly); only a same-file forward reference
# needs this after-the-module placement.
#
# Ported from ActiveDiscussion::Repository's own initial_karma_for:
# the configured item_karma_initial_for callable (see configuration.di)
# turns a persona handle into a starting karma value -- deliberately
# not calling ItemKarma.initial_from_persona_state directly here, since
# that ties this package straight to packages/active_karma's own
# PersonaState. A host app's own callable is free to build on
# ItemKarma.initial_from_persona_state internally (bridging to a real
# ActiveKarma::PersonaState it already loaded), but active_discussion
# itself stays decoupled from any particular karma system, exactly
# like the Ruby original.
def initial_karma_for(persona_handle)
  if persona_handle == nil || persona_handle == ""
    return 0
  end
  callable = ActiveDiscussion::Configuration.item_karma_initial_for()
  karma = if callable == nil then 0 else callable(persona_handle) end
  ActiveDiscussion::ItemKarma.clamp_default(karma)
end

module ActiveDiscussion

# Ported from ActiveDiscussion's own Comment -- threaded (via
# `parent_id`/`depth`, not a materialized path), Slashdot-style
# item-karma-gated visibility. Deliberately drops the canvas/pinboard
# fields (`canvas_id`, `z_index`, `spin_deg`, `x`, `y`) the Ruby
# original's Comment carries on every instance, inherited from
# ActiveStenographer's own "recording placed on a canvas" storage
# primitive -- meaningless for a plain threaded comment (see
# README.md), and a real leaky-UI-metaphor finding from the
# architecture review this whole port is based on.
class Comment < ActiveRecord::Model
  attr_accessor discussion_id, parent_id, persona_handle: String, body: String, karma, depth, edited_at, created_at, updated_at

  def initialize(attributes: Hash = {})
    super(attributes)
    @discussion_id = attributes["discussion_id"]
    @parent_id = attributes["parent_id"]
    @persona_handle = attributes["persona_handle"]
    @body = attributes["body"]
    @karma = if attributes["karma"] == nil then 0 else attributes["karma"] end
    @depth = if attributes["depth"] == nil then 0 else attributes["depth"] end
    @edited_at = attributes["edited_at"]
    @created_at = attributes["created_at"]
    @updated_at = attributes["updated_at"]
  end

  def to_attributes() = {"discussion_id": @discussion_id, "parent_id": @parent_id,
    "persona_handle": @persona_handle, "body": @body, "karma": @karma, "depth": @depth,
    "edited_at": @edited_at, "created_at": @created_at, "updated_at": @updated_at}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def edited?() -> Bool = @edited_at != nil
  def reply?() -> Bool = @parent_id != nil

  def item_visible?(viewer_handle = nil, moderator: Bool = false) -> Bool
    ItemKarma.visible?(@karma, @persona_handle, viewer_handle, moderator, Configuration.hide_karma_threshold())
  end

  def discussion(db) = self.belongs_to(Discussion.repository()).get(db, @discussion_id)

  def replies(db) -> Array
    rows = Comment.where({"discussion_id": @discussion_id, "parent_id": self.id()}).to_a(db)
    rows.sort_by() do |comment| comment.created_at() * 1000000000 + comment.id() end
  end

  # Every descendant, this comment included -- a BFS over parent_id
  # rather than the Ruby original's materialized-path subtree query,
  # since there's no path column here to query against (see the class
  # comment above).
  def subtree_ids(db) -> Array
    ids = [self.id()]
    index = 0
    while index < ids.length()
      children = Comment.where({"parent_id": ids[index]}).to_a(db)
      children.each() do |child|
        ids.push(child.id())
      end
      index += 1
    end
    ids
  end

  def edit!(db, body: String)
    trimmed = body.strip()
    errors = build_comment_body_validator()({"persona_handle": @persona_handle, "body": trimmed}, nil)
    unless errors.empty?()
      raise ActiveRecord::ValidationError.new(errors)
    end
    self.body = trimmed
    self.edited_at = Time.now().to_i()
    self.updated_at = Time.now().to_i()
    self.save(db)
  end

  # Deletes this comment and every reply beneath it.
  def delete!(db)
    ids = self.subtree_ids(db)
    ids.each() do |id|
      Comment.repository().delete(db, id)
    end
  end

  def vote_karma!(db, voter_handle, value) = apply_karma_vote!(db, "comment", self, voter_handle, value)
  def clear_karma_vote!(db, voter_handle) = clear_karma_vote_for!(db, "comment", self, voter_handle)
end

end

def build_comment(row) = ActiveDiscussion::Comment.new(row)

def build_comment_body_validator()
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("persona_handle"),
    ActiveRecord::Validators.presence("body"),
    ActiveRecord::Validators.length("body", 1, 10000),
  ])
end
