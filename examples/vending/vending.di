# vending: drive a coin-operated vending machine from a script.
#
# vending.di SCRIPT
#
# SCRIPT holds one JSON command per line, e.g.
#   {"cmd": "coin", "cents": 25}     {"cmd": "select", "slot": "A1"}
#   {"cmd": "refund"}                {"cmd": "restock", "slot": "A1", "count": 3, "price": 75}
#   {"cmd": "service", "key": "1234"}
# Blank lines and lines starting with # are skipped. Each command prints the
# machine's response; a malformed command is reported and skipped.
require "./lib/machine"
require "./lib/parse"

def usage() -> Int
  warn("usage: vending.di SCRIPT")
  64
end

def new_machine() -> Machine
  Machine.new({"A1": [75, 2], "B2": [125, 1]}, "1234")
end

def run(path: String) -> Int
  machine = new_machine()
  state = Idle.new()
  bad = 0
  File.read(path).split("\n").each_with_index() do |line, index|
    text = line.strip()
    next if text.empty?() || text.start_with?("#")
    begin
      event = parse_command(text)
      [state, messages] = machine.step(state, event)
      messages.each() do |message| puts("#{index + 1}: #{message}") end
    rescue error: CommandError
      warn("#{index + 1}: #{error.message()}")
      bad += 1
    end
  end
  puts("stock: #{machine.slots().map() do |slot| machine.stock_line(slot) end.join("; ")}")
  bad == 0 ? 0 : 1
end

def main(argv) -> Int
  return usage() unless argv.length() == 1
  begin
    run(argv[0])
  rescue error: IOError
    warn(error.message())
    66
  end
end

exit(main(ARGV))
