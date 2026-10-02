# Running totals for one pass over the input, and the text report.

struct Latency(count: Int, p50: Float, p95: Float, p99: Float, max: Float)
end

# Nearest-rank percentile of an ascending, non-empty list: the smallest value
# such that at least `percent`% of the list is <= it. That rank is
# ceil(percent * n / 100), written as integer arithmetic: adding 99 before
# dividing by 100 rounds up. (Rank is 1-based; the list is 0-based.)
def logstat_percentile(sorted: Array[Float], percent: Int) -> Float
  rank = (percent * sorted.length() + 99) / 100

  # A percent of 0 would give rank 0; clamp to the first element.
  if rank < 1 then rank = 1 end
  sorted[rank - 1]
end

# Summarizes request durations. Sorted once, shared by all four lookups.
def logstat_latency(durations: Array[Float]) -> Latency
  sorted = durations.sort()
  Latency.new(sorted.length(), logstat_percentile(sorted, 50), logstat_percentile(sorted, 95),
    logstat_percentile(sorted, 99), sorted[sorted.length() - 1])
end

# One decimal place, e.g. 12.34 -> "12.3".
def logstat_ms(value: Float) -> String = value.round(1).to_s()

# counts[key] += 1, but a missing key starts at 1 (there is no default value
# in a plain Hash).
def logstat_increment(counts, key)
  counts[key] = if counts[key] == nil then 1 else counts[key] + 1 end
end

# Everything learned in one pass. `record` is called once per input line;
# `render` turns the totals into the report.
class Tally
  def initialize()
    # @levels: level -> count. @messages: "tag message" -> count.
    # @statuses: counts by HTTP status class. @durations: every request's
    # latency (kept in full, since percentiles need the whole list).
    @lines = 0
    @levels = {}
    @messages = {}
    @statuses = {"2xx": 0, "3xx": 0, "4xx": 0, "5xx": 0, "other": 0}
    @durations = []
    @unparsed = []
  end

  def unparsed_count() = @unparsed.length()

  # Folds one classified line into the totals. The `case` is exhaustive over
  # the sealed LogLine: a new kind of line would not compile until handled.
  def record(line: LogLine)
    @lines += 1

    case line
    when RequestLine
      logstat_increment(@levels, line.level())
      logstat_increment(@messages, "#{line.tag()} request.completed")

      # Bucket the status by hundreds: 404 / 100 = 4 -> "4xx". Anything
      # outside 2xx-5xx (1xx, or nonsense like 999 or -1) is "other".
      status_class = line.status() / 100
      key = if status_class >= 2 && status_class <= 5 then "#{status_class}xx" else "other" end
      @statuses[key] = @statuses[key] + 1
      @durations.push(line.duration_ms())
    # Other events count toward levels and the message tally only.
    when EventLine
      logstat_increment(@levels, line.level())
      logstat_increment(@messages, "#{line.tag()} #{line.message()}")
    # Remember bad lines (with numbers) for the report's last section.
    when Unparsed
      @unparsed.push(line)
    end
  end

  # Known levels in severity order, then any others alphabetically.
  def level_order()
    # Only the standard levels that actually occurred...
    known = ["debug", "info", "warn", "error", "fatal"].select() do |level|
      @levels[level] != nil
    end

    # ...followed by any non-standard ones.
    others = @levels.keys().select() do |level|
      !known.include?(level)
    end
    known.concat(others.sort())
  end

  # The `limit` most frequent messages as [message, count] pairs. Sorting by
  # the NEGATED count gives descending order with an ascending sort.
  def top_messages(limit: Int)
    pairs = @messages.keys().map() do |key|
      [key, @messages[key]]
    end
    ranked = pairs.sort_by() do |pair|
      -pair[1]
    end
    if ranked.length() > limit then ranked = ranked.take(limit) end
    ranked
  end

  # Builds the whole report as one string. Each section is skipped when it
  # would be empty, so a log with no requests has no "Requests" block.
  def render(limit: Int) -> String
    out = StringBuilder.new()
    out.append("#{@lines} lines, #{@unparsed.length()} unparsed\n")

    # Section: counts per level.
    if @levels.length() > 0
      out.append("\nLevels\n")
      self.level_order().each() do |level|
        out.append("  #{level.ljust(8, " ")}#{@levels[level].to_s().rjust(8, " ")}\n")
      end
    end

    # Section: request statuses and latency percentiles.
    if @durations.length() > 0
      latency = logstat_latency(@durations)
      out.append("\nRequests: #{latency.count()}\n")
      out.append("  2xx #{@statuses["2xx"]}  3xx #{@statuses["3xx"]}  4xx #{@statuses["4xx"]}  5xx #{@statuses["5xx"]}")
      if @statuses["other"] > 0 then out.append("  other #{@statuses["other"]}") end
      out.append("\n  latency ms  p50 #{logstat_ms(latency.p50())}  p95 #{logstat_ms(latency.p95())}  p99 #{logstat_ms(latency.p99())}  max #{logstat_ms(latency.max())}\n")
    end

    # Section: the most common messages.
    top = self.top_messages(limit)
    if top.length() > 0
      out.append("\nTop messages\n")
      top.each() do |pair|
        out.append("  #{pair[1].to_s().rjust(8, " ")}  #{pair[0]}\n")
      end
    end

    # Section: the first few bad lines, with a count of how many were cut.
    if @unparsed.length() > 0
      out.append("\nUnparsed lines\n")
      shown = if @unparsed.length() > limit then @unparsed.take(limit) else @unparsed end
      shown.each() do |line|
        out.append("  line #{line.number()}: #{line.reason()}\n")
      end
      if @unparsed.length() > shown.length()
        out.append("  ... #{@unparsed.length() - shown.length()} more\n")
      end
    end

    out.to_s()
  end
end
