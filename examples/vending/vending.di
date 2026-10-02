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

# A fixed starting inventory: slot => [price in cents, count]. The service
# key is "1234".
def new_machine() -> Machine
  Machine.new({"A1": [75, 2], "B2": [125, 1]}, "1234")
end

# Replays a script. The only mutable state is `state`, replaced after every
# command by the state `step` returns. Returns 0 if every command was valid,
# 1 if any was skipped as malformed.
def run(path: String) -> Int
  machine = new_machine()
  state = Idle.new()
  bad = 0

  File.read(path).split("\n").each_with_index() do |line, index|
    # Skip blank lines and `#` comments (still counted, so the line numbers
    # in the output match the script file).
    text = line.strip()
    next if text.empty?() || text.start_with?("#")

    begin
      # Parse, step, and print each response message prefixed by its line
      # number. `[state, messages] = ...` assigns the new state and unpacks
      # the messages in one go.
      event = parse_command(text)
      [state, messages] = machine.step(state, event)
      messages.each() do |message| puts("#{index + 1}: #{message}") end
    rescue error: CommandError
      warn("#{index + 1}: #{error.message()}")
      bad += 1
    end
  end

  # Finish with the remaining inventory, one summary line.
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
