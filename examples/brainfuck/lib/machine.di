# The Brainfuck machine's state and the meaning of each instruction. The
# interpreter loop itself lives in interpreter.di and is a self-recursive
# tail call; everything it needs to change lives here, in one mutable
# object, so that the loop's own arguments can stay down to a program
# counter and a step count.

# A bad program (unmatched bracket), a pointer off the tape, or too many
# steps.
class BrainfuckError < StandardError
end

class Machine
  attr_reader output: Array

  # The tape is 30000 zeroed cells, the classic size. @ptr is the current
  # cell. @output collects the byte values printed so far.
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
    # Move the pointer; leaving the tape is an error rather than silently
    # wrapping.
    when ">"
      @ptr += 1
      raise BrainfuckError.new("pointer ran off the right end of the tape at instruction #{pc}") if @ptr >= @cells.length()
    when "<"
      @ptr -= 1
      raise BrainfuckError.new("pointer ran off the left end of the tape at instruction #{pc}") if @ptr < 0
    # Cells are bytes and wrap around: 255 + 1 is 0, and 0 - 1 is 255. The
    # "- 1" is written as "+ 255" so the remainder is never negative.
    when "+"
      @cells[@ptr] = (@cells[@ptr] + 1) % 256
    when "-"
      @cells[@ptr] = (@cells[@ptr] + 255) % 256
    # Output is kept as numbers and converted to text at the end.
    when "."
      @output.push(@cells[@ptr])
    # Input: the next byte of the given text, or 0 once it runs out.
    when ","
      if @in_pos < @input.length()
        @cells[@ptr] = @input[@in_pos].ord()
        @in_pos += 1
      else
        @cells[@ptr] = 0
      end
    # `[` skips to just after its matching `]` when the cell is 0; `]` jumps
    # back to just after its matching `[` when the cell is NOT 0. Together
    # they make a while-loop. Every other case falls through to the next
    # instruction.
    when "["
      return jumps[pc] + 1 if @cells[@ptr] == 0
    when "]"
      return jumps[pc] + 1 unless @cells[@ptr] == 0
    end

    pc + 1
  end

  # The output as a string (each number becomes the character with that
  # code).
  def text() -> String
    @output.map() do |code| chr(code) end.join("")
  end
end
