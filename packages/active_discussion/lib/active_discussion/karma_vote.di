module ActiveDiscussion

  class KarmaVote < ActiveRecord::Model
    attr_accessor target_type: String, target_id, voter_handle: String, value, applied_delta

    def initialize(attributes: Hash = {})
      super(attributes)
      @target_type = attributes["target_type"]
      @target_id = attributes["target_id"]
      @voter_handle = attributes["voter_handle"]
      @value = attributes["value"]
      @applied_delta = attributes["applied_delta"]
    end

    def to_attributes() = {"target_type": @target_type, "target_id": @target_id,
      "voter_handle": @voter_handle, "value": @value, "applied_delta": @applied_delta}
    def repository() = @@repository
    def self.repository() = @@repository
    def self.configure(repository: ActiveRecord::Repository)
      @@repository = repository
    end
  end

  end

  def build_karma_vote(row) = ActiveDiscussion::KarmaVote.new(row)

  # Shared top-level helper behind Discussion#vote_karma!/#clear_karma_vote!
  # and Comment#vote_karma!/#clear_karma_vote! (see discussion.di/comment.di)
  # -- ported from ActiveDiscussion::Repository's own vote_karma!/
  # clear_karma_vote!, generalized over `target_type` ("discussion" or
  # "comment") instead of Ruby's `case target when Discussion ... when
  # Comment ... end` dispatch, since both target classes share the exact
  # same shape here (a plain `karma: Int` column, #id/#karma/#karma=/#save)
  # -- no polymorphic dispatch needed at all, just one shared function.
  #
  # Each voter has at most one vote (+1/-1) per target; karma moves in
  # single-step increments and stays clamped to [ItemKarma::MIN,
  # ItemKarma::MAX] (see item_karma.di). `applied_delta` on the stored
  # KarmaVote is *not* just `value` -- it's however much karma actually
  # moved after clamping, so a later re-vote or un-vote can correctly
  # reverse exactly what this vote actually applied, not assume a full
  # +-1 step that clamping may have partially or fully absorbed.
  #
  # Genuine top-level functions, not nested inside `module
  # ActiveDiscussion` above -- see packages/active_auth/README.md for
  # why (a top-level function referencing a namespaced class needs that
  # class already declared earlier in the same file); placed after the
  # module for that reason.
  def apply_karma_vote!(db, target_type, target, voter_handle, value)
    unless value == 1 || value == -1
      raise ArgumentError.new("vote must be +1 or -1")
    end
    existing = ActiveDiscussion::KarmaVote.where(
      {"target_type": target_type, "target_id": target.id(), "voter_handle": voter_handle}).first(db)
    old_applied_delta = if existing == nil then 0 else existing.applied_delta() end
    baseline_karma = ActiveDiscussion::ItemKarma.clamp(target.karma() - old_applied_delta)
    new_karma = ActiveDiscussion::ItemKarma.clamp(baseline_karma + value)
    applied_delta = new_karma - baseline_karma
    if existing == nil
      ActiveDiscussion::KarmaVote.create(db,
        {"target_type": target_type, "target_id": target.id(), "voter_handle": voter_handle,
         "value": value, "applied_delta": applied_delta})
    else
      existing.value = value
      existing.applied_delta = applied_delta
      existing.save(db)
    end
    target.karma = new_karma
    target.save(db)
    target
  end

  def clear_karma_vote_for!(db, target_type, target, voter_handle)
    existing = ActiveDiscussion::KarmaVote.where(
      {"target_type": target_type, "target_id": target.id(), "voter_handle": voter_handle}).first(db)
    if existing == nil
      return target
    end
    new_karma = ActiveDiscussion::ItemKarma.clamp(target.karma() - existing.applied_delta())
    existing.destroy(db)
    target.karma = new_karma
    target.save(db)
    target
end
