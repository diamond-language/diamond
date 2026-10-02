# One JSON object per line becomes an Event. Every command is a Hash
# pattern; `**rest` collects any key the pattern didn't name, and a guard
# rejects the command if there are any, so a typo'd key is an error rather
# than silently ignored.
require "./model"

# A script line the machine cannot make sense of; reported, then skipped.
class CommandError < StandardError
end

# Turns one JSON line into an Event, or raises CommandError.
def parse_command(line: String) -> Event
  # `begin ... rescue ... end` is an expression: it yields the parsed value,
  # or the rescue clause raises instead.
  data = begin
    JSON.parse(line)
  rescue error: JSONError
    raise CommandError.new("not JSON: #{error.message()}")
  end
  # Each arm is a Hash pattern: it matches an object containing these keys
  # (extra keys allowed) and binds the values. `**rest` captures the extras;
  # a guard then insists there are none, and checks the value types.
  case data
  when {"cmd": "coin", "cents": cents, **rest} if rest.empty?() && cents is Int
    Coin.new(cents)
  when {"cmd": "select", "slot": slot, **rest} if rest.empty?() && slot is String
    Select.new(slot)
  # "refund" and "cancel" are synonyms (two patterns, one arm). No guard
  # here, so extra keys are rejected explicitly inside the arm.
  when {"cmd": "refund", **rest}, {"cmd": "cancel", **rest}
    raise CommandError.new("refund takes no arguments") unless rest.empty?()
    Refund.new()
  when {"cmd": "restock", "slot": slot, "count": count, "price": price, **rest} if rest.empty?() && count is Int && price is Int
    Restock.new(slot, count, price)
  when {"cmd": "service", "key": key, **rest} if rest.empty?() && key is String
    Service.new(key)
  # A known command that failed its arm's guard above (wrong or missing
  # arguments), versus a command name that does not exist at all.
  when {"cmd": name, **rest}
    if ["coin", "select", "restock", "service"].include?(name)
      raise CommandError.new("bad arguments for #{name}: #{JSON.stringify(rest)}")
    end
    raise CommandError.new("unknown command '#{name}'")
  # Valid JSON, but not an object with a "cmd" key (an array, a number...).
  else
    raise CommandError.new("expected an object with a \"cmd\" key")
  end
end
