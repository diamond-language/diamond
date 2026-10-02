# Runs Brainfuck. The heart is `execute`, whose every step is a
# self-recursive tail call:
#
#     return execute(machine, code, jumps, next_pc, steps + 1)
#
# A program of a few hundred thousand instructions therefore runs in one
# constant-size call frame. Without the optimization each instruction would
# be a nested call, and the ordinary depth limit of 95 would stop even a
# hello world. The call qualifies because it is the whole value of a
# `return` in a plain (non-generic, non-variadic) function, outside any
# begin/rescue; see depth.di for what happens when it isn't.
require "./machine"

# Instructions only; everything else in the source is a comment.
def parse_program(source: String) -> Array
  # Keep only the eight instruction characters; `include?` tests membership
  # in the string of valid instructions.
  source.chars().select() do |c| "<>+-.,[]".include?(c) end
end

# Matches each bracket with its partner, so a jump is one array lookup.
def jump_table(code: Array) -> Array
  # One entry per instruction; -1 for everything that is not a bracket.
  jumps = code.map() do |op| -1 end

  # A stack of the `[` positions not yet closed. Each `]` closes the most
  # recent one (brackets nest), and the pair is recorded in BOTH directions.
  open = []

  code.each_with_index() do |op, pc|
    if op == "["
      open.push(pc)
    elsif op == "]"
      raise BrainfuckError.new("unmatched ] at instruction #{pc}") if open.empty?()
      partner = open.pop()
      jumps[partner] = pc
      jumps[pc] = partner
    end
  end

  # Anything left on the stack is a `[` that was never closed.
  raise BrainfuckError.new("unmatched [ at instruction #{open.last()}") unless open.empty?()
  jumps
end

# Returns the number of instructions executed. The step limit is a real
# safeguard, not decoration: a self-recursive tail call that never
# terminates now runs forever instead of eventually overflowing the stack.
def execute(machine: Machine, code: Array, jumps: Array, pc: Int, steps: Int, limit: Int) -> Int
  # Done: ran off the end of the program.
  return steps if pc >= code.length()

  # Safeguard against programs that loop forever.
  raise BrainfuckError.new("step limit of #{limit} exceeded (at instruction #{pc})") if steps >= limit

  # Perform one instruction (which returns where to go next), then continue.
  # This `return execute(...)` is the tail call.
  next_pc = machine.apply(code[pc], jumps, pc)
  return execute(machine, code, jumps, next_pc, steps + 1, limit)
end

# Parses, links the brackets, and runs from instruction 0 with 0 steps taken.
# Returns [output text, instruction count].
def run_program(source: String, input: String, limit: Int) -> Array
  code = parse_program(source)
  machine = Machine.new(input)
  steps = execute(machine, code, jump_table(code), 0, 0, limit)
  [machine.text(), steps]
end
