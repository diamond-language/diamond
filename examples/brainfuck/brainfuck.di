# brainfuck: an interpreter whose main loop is a self-recursive tail call.
#
# brainfuck.di FILE [--input TEXT] [--limit N] [--steps]
#
# --input TEXT   the bytes `,` reads (0 once they run out)
# --limit N      stop after N instructions (default 10000000)
# --steps        also report how many instructions ran, on stderr
require "./lib/interpreter"

def usage() -> Int
  warn("usage: brainfuck.di FILE [--input TEXT] [--limit N] [--steps]")
  64
end

def run(argv: Array) -> Int
  path = nil
  input = ""
  limit = 10_000_000
  show_steps = false
  index = 0
  while index < argv.length()
    case argv[index]
    when "--input"
      index += 1
      return usage() if index >= argv.length()
      input = argv[index]
    when "--limit"
      index += 1
      return usage() if index >= argv.length() || argv[index].to_i() < 1
      limit = argv[index].to_i()
    when "--steps" then show_steps = true
    else
      return usage() unless path == nil
      path = argv[index]
    end
    index += 1
  end
  return usage() if path == nil

  [text, steps] = run_program(File.read(path), input, limit)
  print(text)
  warn("#{steps} instructions") if show_steps
  0
end

def main(argv) -> Int
  begin
    run(argv)
  rescue error: BrainfuckError
    warn(error.message())
    65
  rescue error: IOError
    warn(error.message())
    66
  end
end

exit(main(ARGV))
