# The Brainfuck machine's state and the meaning of each instruction. The
# interpreter loop itself lives in interpreter.di and is a self-recursive
# tail call; everything it needs to change lives here, in one mutable
# object, so that the loop's own arguments can stay down to a program
# counter and a step count.

class BrainfuckError < StandardError
end

class Machine
  attr_reader output: Array

  def initialize(input: String, tape_size: Int = 30000)
    @cells = (0...tape_size).map() do |i| 0 end
    @ptr = 0
    @input = input
    @in_pos = 0
    @output = []
  end

  # Performs the instruction at `pc` and returns the pc to run next.
  # `jumps[pc]` is the matching bracket for a `[` or `]`.
  def apply(op: String, jumps: Array, pc: Int) -> Int
    case op
    when ">"
      @ptr += 1
      raise BrainfuckError.new("pointer ran off the right end of the tape at instruction #{pc}") if @ptr >= @cells.length()
    when "<"
      @ptr -= 1
      raise BrainfuckError.new("pointer ran off the left end of the tape at instruction #{pc}") if @ptr < 0
    when "+"
      @cells[@ptr] = (@cells[@ptr] + 1) % 256
    when "-"
      @cells[@ptr] = (@cells[@ptr] + 255) % 256
    when "."
      @output.push(@cells[@ptr])
    when ","
      if @in_pos < @input.length()
        @cells[@ptr] = @input[@in_pos].ord()
        @in_pos += 1
      else
        @cells[@ptr] = 0
      end
    when "["
      return jumps[pc] + 1 if @cells[@ptr] == 0
    when "]"
      return jumps[pc] + 1 unless @cells[@ptr] == 0
    end
    pc + 1
  end

  def text() -> String
    @output.map() do |code| chr(code) end.join("")
  end
end
