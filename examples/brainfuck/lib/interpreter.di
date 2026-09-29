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
  source.chars().select() do |c| "<>+-.,[]".include?(c) end
end

# Matches each bracket with its partner, so a jump is one array lookup.
def jump_table(code: Array) -> Array
  jumps = code.map() do |op| -1 end
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
  raise BrainfuckError.new("unmatched [ at instruction #{open.last()}") unless open.empty?()
  jumps
end

# Returns the number of instructions executed. The step limit is a real
# safeguard, not decoration: a self-recursive tail call that never
# terminates now runs forever instead of eventually overflowing the stack.
def execute(machine: Machine, code: Array, jumps: Array, pc: Int, steps: Int, limit: Int) -> Int
  return steps if pc >= code.length()
  raise BrainfuckError.new("step limit of #{limit} exceeded (at instruction #{pc})") if steps >= limit
  next_pc = machine.apply(code[pc], jumps, pc)
  return execute(machine, code, jumps, next_pc, steps + 1, limit)
end

def run_program(source: String, input: String, limit: Int) -> Array
  code = parse_program(source)
  machine = Machine.new(input)
  steps = execute(machine, code, jump_table(code), 0, 0, limit)
  [machine.text(), steps]
end
