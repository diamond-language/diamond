# Running totals for one pass over the input, and the text report.

struct Latency(count: Int, p50: Float, p95: Float, p99: Float, max: Float)
end

# Nearest-rank percentile of an ascending, non-empty list.
def logstat_percentile(sorted: Array[Float], percent: Int) -> Float
  rank = (percent * sorted.length() + 99) / 100
  if rank < 1 then rank = 1 end
  sorted[rank - 1]
end

def logstat_latency(durations: Array[Float]) -> Latency
  sorted = durations.sort()
  Latency.new(sorted.length(), logstat_percentile(sorted, 50), logstat_percentile(sorted, 95),
    logstat_percentile(sorted, 99), sorted[sorted.length() - 1])
end

# One decimal place; Float has no rounding methods, so this rounds by hand.
def logstat_ms(value: Float) -> String
  tenths = to_i(value * 10.0 + 0.5)
  "#{tenths / 10}.#{tenths % 10}"
end

def logstat_increment(counts, key)
  counts[key] = if counts[key] == nil then 1 else counts[key] + 1 end
end

class Tally
  def initialize()
    @lines = 0
    @levels = {}
    @messages = {}
    @statuses = {"2xx": 0, "3xx": 0, "4xx": 0, "5xx": 0, "other": 0}
    @durations = []
    @unparsed = []
  end

  def unparsed_count() = @unparsed.length()

  def record(line: LogLine)
    @lines += 1
    case line
    when RequestLine
      logstat_increment(@levels, line.level())
      logstat_increment(@messages, "#{line.tag()} request.completed")
      status_class = line.status() / 100
      key = if status_class >= 2 && status_class <= 5 then "#{status_class}xx" else "other" end
      @statuses[key] = @statuses[key] + 1
      @durations.push(line.duration_ms())
    when EventLine
      logstat_increment(@levels, line.level())
      logstat_increment(@messages, "#{line.tag()} #{line.message()}")
    when Unparsed
      @unparsed.push(line)
    end
  end

  # Known levels in severity order, then any others alphabetically.
  def level_order()
    known = ["debug", "info", "warn", "error", "fatal"].select() do |level|
      @levels[level] != nil
    end
    others = @levels.keys().select() do |level|
      !known.include?(level)
    end
    known.concat(others.sort())
  end

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

  def render(limit: Int) -> String
    out = StringBuilder.new()
    out.append("#{@lines} lines, #{@unparsed.length()} unparsed\n")
    if @levels.length() > 0
      out.append("\nLevels\n")
      self.level_order().each() do |level|
        out.append("  #{level.ljust(8, " ")}#{"#{@levels[level]}".rjust(8, " ")}\n")
      end
    end
    if @durations.length() > 0
      latency = logstat_latency(@durations)
      out.append("\nRequests: #{latency.count()}\n")
      out.append("  2xx #{@statuses["2xx"]}  3xx #{@statuses["3xx"]}  4xx #{@statuses["4xx"]}  5xx #{@statuses["5xx"]}")
      if @statuses["other"] > 0 then out.append("  other #{@statuses["other"]}") end
      out.append("\n  latency ms  p50 #{logstat_ms(latency.p50())}  p95 #{logstat_ms(latency.p95())}  p99 #{logstat_ms(latency.p99())}  max #{logstat_ms(latency.max())}\n")
    end
    top = self.top_messages(limit)
    if top.length() > 0
      out.append("\nTop messages\n")
      top.each() do |pair|
        out.append("  #{"#{pair[1]}".rjust(8, " ")}  #{pair[0]}\n")
      end
    end
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
