# ActiveDiscussion: threaded comments, Slashdot-style item karma, and
# flame-signal reporting -- ported from MaquinasStack's private Ruby
# ActiveDiscussion gem. See README.md for what's ported and what isn't
# (the case/vote moderation tribunal and forum-level permission scheme
# a separate architecture review found duplicating this elsewhere in
# the Ruby monorepo -- neither is ported; this package's own
# FlameSignal is the one moderation primitive kept).
require "../../active_record/lib/active_record"
require "./active_discussion/signal"
require "./active_discussion/item_karma"
require "./active_discussion/configuration"
require "./active_discussion/karma_vote"
require "./active_discussion/flame_signal"
require "./active_discussion/comment"
require "./active_discussion/discussion"
