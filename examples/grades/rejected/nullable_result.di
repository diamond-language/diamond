# best_by returns T | Nil -- nil for an empty Array -- so promising a plain
# Score here is an error until the nil case is handled.
require "../lib/reports"
def top_score(scores: Array[Score]) -> Score
  best_by(scores, score_value)
end
top_score([])
