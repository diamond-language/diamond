# Shared between SkinsController#show and CommentsController: every
# skin has exactly one ActiveDiscussion::Discussion, keyed by
# `recording_id` = "#{skin.id()}" (an opaque String per the package's
# own design -- see packages/active_discussion/README.md). Not
# `Discussion.find_or_open` directly: that has no way to set
# `max_depth` only on first creation, and every comment thread here is
# capped at one level of replies (matching modArtist's own UI, which
# never renders replies-of-replies).
def discussion_for_skin(db, skin_id)
  discussion = ActiveDiscussion::Discussion.where({"recording_id": "#{skin_id}"}).first(db)
  if discussion == nil
    discussion = ActiveDiscussion::Discussion.open!(db, "#{skin_id}")
    discussion.configure_limits!(db, 1)
  end
  discussion
end
