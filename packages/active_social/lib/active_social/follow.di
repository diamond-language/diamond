module ActiveSocial

  # Ported from ActiveSocial's own Repository (the follow-graph half
  # only -- see README.md for why reactions/likes/reposts aren't
  # ported). The Ruby original stores follows as two event-sourced
  # recordings per relationship (an outbound one under the follower, an
  # inbound one under the followed, "last event wins" for active/
  # inactive state) via ActiveStenographer, which isn't ported yet (see
  # packages/active_karma/README.md for why porting a slice of it just
  # for one consumer would reproduce the foundation-depends-on-consumers
  # problem the architecture review flagged). A plain `follows` table --
  # one row per active relationship, deleted on unfollow -- answers the
  # same two query directions (`WHERE follower_id = x` /
  # `WHERE followed_id = x`) without needing the double-write/event-log
  # machinery at all; this is a simplification the storage swap makes
  # possible, not a missing feature.
  #
  # `follower_id`/`followed_id` are opaque ids here, exactly like the
  # Ruby original's own string `persona_id`s -- this package doesn't
  # assume or enforce which table they reference; a consuming app's own
  # schema (typically an accounts/personas table) owns that FK.
  class Follow < ActiveRecord::Model
    attr_accessor follower_id, followed_id, created_at

    def initialize(attributes: Hash = {})
      super(attributes)
      @follower_id = attributes["follower_id"]
      @followed_id = attributes["followed_id"]
      @created_at = attributes["created_at"]
    end

    def to_attributes() = {"follower_id": @follower_id, "followed_id": @followed_id, "created_at": @created_at}
    def repository() = @@repository
    def self.repository() = @@repository
    def self.configure(repository: ActiveRecord::Repository)
      @@repository = repository
    end

    def self.follows?(db, follower_id, followed_id) -> Bool
      Follow.where({"follower_id": follower_id, "followed_id": followed_id}).first(db) != nil
    end

    # Idempotent, matching the Ruby original: following someone you
    # already follow is a no-op (returns false), not an error or a
    # duplicate row.
    def self.follow!(db, follower_id, followed_id) -> Bool
      if Follow.follows?(db, follower_id, followed_id)
        return false
      end
      Follow.create(db, {"follower_id": follower_id, "followed_id": followed_id, "created_at": Time.now().to_i()})
      true
    end

    # Also idempotent: unfollowing someone you don't follow is a no-op.
    def self.unfollow!(db, follower_id, followed_id) -> Bool
      existing = Follow.where({"follower_id": follower_id, "followed_id": followed_id}).first(db)
      if existing == nil
        return false
      end
      existing.destroy(db)
      true
    end

    # The ids of every persona `persona_id` follows.
    def self.following(db, persona_id) -> Array
      rows = Follow.where({"follower_id": persona_id}).to_a(db)
      ids = []
      rows.each() do |row| ids.push(row.followed_id()) end
      ids
    end

    # The ids of every persona following `persona_id`.
    def self.followers(db, persona_id) -> Array
      rows = Follow.where({"followed_id": persona_id}).to_a(db)
      ids = []
      rows.each() do |row| ids.push(row.follower_id()) end
      ids
    end

    def self.following_count(db, persona_id) -> Int = Follow.following(db, persona_id).length()
    def self.followers_count(db, persona_id) -> Int = Follow.followers(db, persona_id).length()
  end

end

# A genuine top-level function, not nested inside `module ActiveSocial`
# above -- a top-level function referencing a namespaced class needs
# that class already declared earlier in the same file (confirmed
# porting packages/active_karma/active_auth; see either package's own
# README), so this is placed after the module rather than before.
def build_follow(row) = ActiveSocial::Follow.new(row)
