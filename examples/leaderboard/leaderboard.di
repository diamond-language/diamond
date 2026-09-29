# leaderboard: replay a log of score events into a ranked board.
#
# leaderboard.di FILE [--top N] [--quiet] [--versus FILE2]
#
# FILE has one `name score` pair per line (# starts a comment). Scores are
# replayed in order; a player's board score is their best. While replaying,
# rank changes are printed as they happen (--quiet turns that off), then
# the final ranking, some statistics, and, with --versus, a comparison
# against a second log's board.
require "./lib/board"
require "./lib/ranking"

def usage() -> Int
  warn("usage: leaderboard.di FILE [--top N] [--quiet] [--versus FILE2]")
  64
end

class LogError < StandardError
end

def load_board(path: String, title: String, listener) -> Leaderboard
  board = Leaderboard.new(title)
  board.subscribe(listener) unless listener == nil
  File.read(path).split("\n").each_with_index() do |line, index|
    text = line.strip()
    next if text.empty?() || text.start_with?("#")
    fields = text.split(" ").reject() do |w| w.empty?() end
    unless fields.length() == 2 && fields[1].to_i().to_s() == fields[1]
      raise LogError.new("#{path}:#{index + 1}: expected `name score`, got `#{text}`")
    end
    board.record(fields[0], fields[1].to_i())
  end
  board
end

def describe(event: Symbol, details: Array) -> String
  case [event, details]
  when [:joined, [name, score]] then "#{name} joins with #{score}"
  when [:improved, [name, score]] then "#{name} improves to #{score}"
  when [:took_lead, [name, previous]] then "#{name} takes the lead from #{previous}"
  else "?"
  end
end

def print_ranking(board: Leaderboard, top: Int)
  puts("#{board.title()}: #{board.size()} players")
  board.take(top).each_with_index() do |entry, index|
    medal = Ranking.medal(index + 1)
    suffix = if medal.empty?() then "" else "  (#{medal})" end
    puts("#{(index + 1).to_s().rjust(3, " ")}  #{entry.name().ljust(10, " ")} #{entry.score().to_s().rjust(5, " ")}#{suffix}")
  end
  hidden = board.size() - top
  puts("     ... and #{hidden} more") if hidden > 0
end

def print_stats(board: Leaderboard)
  scores = board.map() do |entry| entry.score() end
  return if scores.empty?()
  total = scores.sum()
  puts("")
  puts("total #{total}, mean #{total / scores.length()}, best #{board.max().name()}, lowest #{board.min().name()}")
  [above, below] = board.partition() do |entry| entry.score() * scores.length() > total end
  puts("above the mean: #{above.map() do |e| e.name() end.join(", ")}")
  puts("at or below:    #{below.map() do |e| e.name() end.join(", ")}")
  tiers = board.group_by() do |entry| Ranking.tier(entry.score()) end
  ["champion", "regular", "rookie"].each() do |tier|
    members = tiers.fetch(tier, [])
    puts("#{tier.ljust(9, " ")} #{members.length()}") unless members.empty?()
  end
  puts("pages of 3: #{board.each_slice(3).map() do |page| page.map() do |e| e.name() end.join("+") end.join(" | ")}")
end

def run(argv: Array) -> Int
  path = nil
  top = 5
  quiet = false
  versus = nil
  index = 0
  while index < argv.length()
    case argv[index]
    when "--top"
      index += 1
      return usage() if index >= argv.length() || argv[index].to_i() < 1
      top = argv[index].to_i()
    when "--quiet" then quiet = true
    when "--versus"
      index += 1
      return usage() if index >= argv.length()
      versus = argv[index]
    else
      return usage() unless path == nil
      path = argv[index]
    end
    index += 1
  end
  return usage() if path == nil

  listener = nil
  unless quiet
    listener = nested_listener()
  end
  board = load_board(path, File.basename(path), listener)
  puts("") unless quiet
  print_ranking(board, top)
  print_stats(board)
  unless versus == nil
    other = load_board(versus, File.basename(versus), nil)
    puts("")
    verdict = if board.outscores?(other) then "outscores"
              elsif other.outscores?(board) then "is outscored by"
              else "ties with" end
    puts("#{board.title()} #{verdict} #{other.title()}")
  end
  0
end

# A nested `def` is a closure value: returning it hands the board a
# callback it can invoke as `listener(event, details)`.
def nested_listener()
  def listener(event, details)
    puts(describe(event, details))
  end
  listener
end

def main(argv) -> Int
  begin
    run(argv)
  rescue error: LogError
    warn(error.message())
    65
  rescue error: IOError
    warn(error.message())
    66
  end
end

exit(main(ARGV))
