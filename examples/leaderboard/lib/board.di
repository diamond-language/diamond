# A leaderboard of each player's best score, ranked highest first.
#
# `include Enumerable` plus one `each` gives the whole Enumerable toolbox
# (sum, min_by, group_by, partition, each_slice, take, ...) over ranked
# entries. `include Observable` lets callers watch rank changes as they
# happen. `protected` lets two boards compare their hidden totals without
# exposing that total to anyone else.
require "./observable"

# Highest score is the "greatest" entry; ties break toward the name that
# sorts first, so a ranking is always fully determined.
struct Entry(name: String, score: Int)
  include Comparable

  def <=>(other: Entry)
    return @score <=> other.score() unless @score == other.score()
    other.name() <=> @name
  end

  # Comparable's derived == would hand `x == nil` to <=>, which insists on
  # an Entry. A struct's own def == replaces its generated field-by-field
  # one, so this answers false for anything that isn't an Entry.
  def ==(other)
    other is Entry && @score == other.score() && @name == other.name()
  end
end

class Leaderboard
  include Enumerable
  include Observable

  def initialize(title: String)
    @title = title
    @best = {}
  end

  def title() -> String = @title
  def size() -> Int = @best.length()

  # Records a score. Only a personal best changes the board, and only a
  # change is announced: :joined for a first score, :improved for a new
  # personal best, and :took_lead when that lifts the player to first place.
  def record(name: String, score: Int) -> Bool
    leader_before = self.leader_name()
    known = @best.include_key?(name)
    return false if known && score <= @best[name]
    @best[name] = score
    self.announce(known ? :improved : :joined, [name, score])
    leader_now = self.leader_name()
    if leader_now == name && leader_before != name && leader_before != nil
      self.announce(:took_lead, [name, leader_before])
    end
    true
  end

  # Ranked entries, best first. This one method is all Enumerable needs.
  def each(callback)
    ranked().each(callback)
    self
  end

  def rank_of(name: String) -> Int | Nil
    position = ranked().index_of(ranked().find() do |entry| entry.name() == name end)
    if position == nil then nil else position + 1 end
  end

  def leader_name() -> String | Nil
    top = ranked().first_or(nil)
    if top == nil then nil else top.name() end
  end

  # True when this board's combined score beats the other's. Reads the
  # other board's protected `total`, which only a Leaderboard may do.
  def outscores?(other: Leaderboard) -> Bool
    self.total() > other.total()
  end

  protected

  def total() -> Int = @best.values().sum()

  private

  def ranked() -> Array
    entries = @best.keys().map() do |name| Entry.new(name, @best[name]) end
    entries.sort().reverse()
  end
end
