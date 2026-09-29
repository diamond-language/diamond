# One JSON object per line becomes an Event. Every command is a Hash
# pattern; `**rest` collects any key the pattern didn't name, and a guard
# rejects the command if there are any, so a typo'd key is an error rather
# than silently ignored.
require "./model"

class CommandError < StandardError
end

def parse_command(line: String) -> Event
  data = begin
    JSON.parse(line)
  rescue error: JSONError
    raise CommandError.new("not JSON: #{error.message()}")
  end
  case data
  when {"cmd": "coin", "cents": cents, **rest} if rest.empty?() && cents is Int
    Coin.new(cents)
  when {"cmd": "select", "slot": slot, **rest} if rest.empty?() && slot is String
    Select.new(slot)
  when {"cmd": "refund", **rest}, {"cmd": "cancel", **rest}
    raise CommandError.new("refund takes no arguments") unless rest.empty?()
    Refund.new()
  when {"cmd": "restock", "slot": slot, "count": count, "price": price, **rest} if rest.empty?() && count is Int && price is Int
    Restock.new(slot, count, price)
  when {"cmd": "service", "key": key, **rest} if rest.empty?() && key is String
    Service.new(key)
  when {"cmd": name, **rest}
    if ["coin", "select", "restock", "service"].include?(name)
      raise CommandError.new("bad arguments for #{name}: #{JSON.stringify(rest)}")
    end
    raise CommandError.new("unknown command '#{name}'")
  else
    raise CommandError.new("expected an object with a \"cmd\" key")
  end
end
