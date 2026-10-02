# calc: an interactive calculator, and a tour of how Diamond handles a
# small language -- a tokenizer, a Pratt parser, a sealed syntax tree,
# pattern-matching rewrites, and errors that point at a column.
#
#   diamond calc.di              # reads lines from stdin
#   echo "2 ^ 100" | diamond calc.di
#
# Each line is an expression, an assignment (x = 3), or a command:
#   :tree EXPR       show how the parser grouped EXPR
#   :simplify EXPR   show EXPR after algebraic simplification
#   :vars            list defined names
require "./lib/parser"
require "./lib/evaluator"

# Prints the line with a caret under the column the error names. For a
# command, the column counts from the start of its expression, after the
# command word and its space.
def calc_report(line: String, error: CalcError)
  # The column is measured within the EXPRESSION, but the caret is drawn
  # under the whole echoed line, so a command's prefix (":tree ") has to be
  # added back on.
  offset = if line.start_with?(":") then line.index_of(" ") + 1 else 0 end

  puts("  #{line}")
  puts("  #{" ".repeat(offset + error.column())}^ #{error.message()}")
end

# Handles one non-blank input line: a `:command`, or an expression to
# evaluate. Anything the parser or evaluator rejects raises a CalcError,
# which calc_main catches.
def calc_line(line: String, evaluator: Evaluator)
  # Dispatch on the line split into words. These are array patterns: `_`
  # matches one word, `*_` any number of extra words, and the `if` on the
  # next-to-last arm is a guard. Order matters: the specific commands come
  # first, the catch-all unknown ":x" next, plain expressions last.
  case line.split(" ")
  when [":vars"]
    puts(evaluator.names().sort().join(" "))
  when [":tree", _, *_]
    # Slice off the command word by length (":tree " is 6 chars,
    # ":simplify " is 10) rather than using the split words, so the
    # expression keeps its original spacing and error columns stay correct.
    puts(calc_show(Parser.parse(line.slice(6, line.length()))))
  when [":simplify", _, *_]
    puts(calc_show(calc_simplify(Parser.parse(line.slice(10, line.length())))))
  # A command with no expression after it.
  when [":tree"], [":simplify"]
    puts("usage: #{line} EXPR")
  when [command, *_] if command.start_with?(":")
    puts("unknown command #{command}")
  else
    puts("=> #{evaluator.eval(Parser.parse(line))}")
  end
end

# The read-eval-print loop over stdin. Returns the process exit code: 1 if
# any line failed, 0 otherwise.
def calc_main() -> Int
  # One Evaluator for the whole session, so variables persist between lines.
  evaluator = Evaluator.new()
  failures = 0

  loop do
    # gets() returns nil at end of input.
    text = gets()
    break if text == nil

    # Skip blank lines and `#` comments (so a session file can be annotated),
    # echo everything else so a transcript reads like a terminal session.
    line = text.strip()
    next if line.empty?() || line.start_with?("#")
    puts("> #{line}")

    # An error in one line must not end the session; report it and carry on.
    begin
      calc_line(line, evaluator)
    rescue error: CalcError
      calc_report(line, error)
      failures += 1
    end
  end

  if failures > 0 then 1 else 0 end
end

exit(calc_main())
