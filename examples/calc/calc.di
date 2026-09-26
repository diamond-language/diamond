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
  offset = if line.start_with?(":") then line.index_of(" ") + 1 else 0 end
  puts("  #{line}")
  puts("  #{" ".repeat(offset + error.column())}^ #{error.message()}")
end

def calc_line(line: String, evaluator: Evaluator)
  case line.split(" ")
  when [":vars"]
    puts(evaluator.names().sort().join(" "))
  when [":tree", _, *_]
    puts(calc_show(Parser.parse(line.slice(6, line.length()))))
  when [":simplify", _, *_]
    puts(calc_show(calc_simplify(Parser.parse(line.slice(10, line.length())))))
  when [":tree"], [":simplify"]
    puts("usage: #{line} EXPR")
  when [command, *_] if command.start_with?(":")
    puts("unknown command #{command}")
  else
    puts("=> #{evaluator.eval(Parser.parse(line))}")
  end
end

def calc_main() -> Int
  evaluator = Evaluator.new()
  failures = 0
  loop do
    text = gets()
    break if text == nil
    line = text.strip()
    next if line.empty?() || line.start_with?("#")
    puts("> #{line}")
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
