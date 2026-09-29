# StatsD-style metric lines and their aggregation.
#
#   name:value|c            counter: values are summed
#   name:value|c|@0.1       counter sampled at 10%, so each counts as value / 0.1
#   name:value|g            gauge: the last value wins
#   name:value|ms           timer: count, min, mean, 90th percentile, max
#
# A datagram can carry several lines separated by newlines.

class MetricError < StandardError
end

struct Sample(name: String, value: Float, kind: String)
end

def number?(text: String) -> Bool
  Regexp.new("^-?[0-9]+(\\.[0-9]+)?$").match(text) != nil
end

def parse_sample(line: String) -> Sample
  [name, rest] = split_once(line, ":")
  unless Regexp.new("^[A-Za-z_][A-Za-z0-9_.]*$").match(name) != nil
    raise MetricError.new("bad metric name '#{name}'")
  end
  parts = rest.split("|")
  raise MetricError.new("expected name:value|type in '#{line}'") if parts.length() < 2 || parts.length() > 3
  raise MetricError.new("bad value '#{parts[0]}'") unless number?(parts[0])
  value = parts[0].to_f()
  kind = parts[1]
  unless ["c", "g", "ms"].include?(kind)
    raise MetricError.new("unknown metric type '#{kind}'")
  end
  if parts.length() == 3
    raise MetricError.new("only counters take a sample rate") unless kind == "c"
    rate = parts[2]
    unless rate.start_with?("@") && number?(rate.slice(1, rate.length())) && rate.slice(1, rate.length()).to_f() > 0.0
      raise MetricError.new("bad sample rate '#{rate}'")
    end
    value = value / rate.slice(1, rate.length()).to_f()
  end
  Sample.new(name, value, kind)
end

# Splits on the first `separator`; a missing one leaves `rest` empty.
def split_once(text: String, separator: String) -> Array
  at = text.index_of(separator)
  return [text, ""] if at == nil
  [text.slice(0, at), text.slice(at + separator.length(), text.length())]
end

class Aggregator
  def initialize()
    @counters = {}
    @gauges = {}
    @timers = {}
  end

  def record(sample: Sample)
    case sample.kind()
    when "c" then @counters[sample.name()] = @counters.fetch(sample.name(), 0.0) + sample.value()
    when "g" then @gauges[sample.name()] = sample.value()
    else
      @timers[sample.name()] = [] unless @timers.include_key?(sample.name())
      @timers[sample.name()].push(sample.value())
    end
  end

  # Parses and records every line of a datagram or file body. Nothing is
  # recorded unless all of it parses, so a bad line rejects the whole batch.
  def record_lines(text: String) -> Int
    samples = text.split("\n").reject() do |l| l.strip().empty?() end.map() do |l|
      parse_sample(l.strip())
    end
    samples.each() do |sample| self.record(sample) end
    samples.length()
  end

  def reset()
    @counters = {}
    @gauges = {}
    @timers = {}
  end

  def empty?() -> Bool = @counters.empty?() && @gauges.empty?() && @timers.empty?()

  # The nearest-rank percentile of an already-sorted Array.
  def percentile(sorted: Array, p: Int) -> Float
    rank = (sorted.length() * p + 99) / 100
    sorted[if rank < 1 then 0 else rank - 1 end]
  end

  def report() -> Hash
    timers = {}
    @timers.keys().sort().each() do |name|
      values = @timers[name].sort()
      timers[name] = {
        "count": values.length(),
        "min": values.first(),
        "mean": values.sum() / values.length(),
        "p90": self.percentile(values, 90),
        "max": values.last()
      }
    end
    {"counters": @counters, "gauges": @gauges, "timers": timers}
  end

  def to_text() -> String
    data = self.report()
    lines = []
    unless data["counters"].empty?()
      lines.push("counters")
      data["counters"].keys().sort().each() do |name|
        lines.push("  #{name.ljust(18, " ")} #{"%.2f".format(data["counters"][name])}")
      end
    end
    unless data["gauges"].empty?()
      lines.push("gauges")
      data["gauges"].keys().sort().each() do |name|
        lines.push("  #{name.ljust(18, " ")} #{"%.2f".format(data["gauges"][name])}")
      end
    end
    unless data["timers"].empty?()
      lines.push("timers (ms)")
      data["timers"].keys().sort().each() do |name|
        t = data["timers"][name]
        lines.push("  #{name.ljust(18, " ")} n=#{t["count"]} min=#{"%.2f".format(t["min"])} mean=#{"%.2f".format(t["mean"])} p90=#{"%.2f".format(t["p90"])} max=#{"%.2f".format(t["max"])}")
      end
    end
    lines.push("(nothing recorded)") if lines.empty?()
    lines.join("\n")
  end
end
