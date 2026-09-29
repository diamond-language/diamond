# A module used as a namespace: `def self.name` inside it is called as
# `Ranking.name(...)`, and never as a bare `name(...)` from outside, so
# these helpers can't hijack a global of the same name.
module Ranking
  def self.medal(rank: Int) -> String
    case rank
    when 1 then "gold"
    when 2 then "silver"
    when 3 then "bronze"
    else ""
    end
  end

  # The tier a score falls in, for grouping.
  def self.tier(score: Int) -> String
    case score
    when 0...100 then "rookie"
    when 100...500 then "regular"
    else "champion"
    end
  end
end
