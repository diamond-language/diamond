# What to look for. Each rule is a named pattern, plus an optional check a
# match must also pass. The built-in rules run first, in order, then any
# from a rules file (see load_rules).
require "./checks"

struct Rule(name: String, pattern: Regexp, check: Callable | Nil)
end

class RulesError < StandardError
end

def builtin_rules() -> Array[Rule]
  [
    Rule.new("private_key", Regexp.new("-----BEGIN [A-Z ]*PRIVATE KEY-----"), nil),
    Rule.new("aws_key", Regexp.new("\\bAKIA[0-9A-Z]{16}\\b"), nil),
    Rule.new("bearer", Regexp.new("(?<=[Bb]earer )[A-Za-z0-9._~+/-]+=*"), nil),
    Rule.new("secret", Regexp.new("(?<=password=|secret=|token=)[^\\s&]+", 1), nil),
    Rule.new("email", Regexp.new("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}"), nil),
    Rule.new("card", Regexp.new("\\b(?:[0-9][ -]?){12,18}[0-9]\\b"), luhn?),
    Rule.new("ipv4", Regexp.new("\\b(?:[0-9]{1,3}\\.){3}[0-9]{1,3}\\b"), ipv4?)
  ]
end

# A rules file has one rule per line: a name, flags ("-" for none, "i" to
# ignore case, "x" for extended), and the pattern, which runs to the end
# of the line. Blank lines and # comments are skipped.
#
#   employee_id  -  \bEMP-[0-9]{6}\b
#   ticket       i  \bticket #[0-9]+
def parse_rule_line(line: String, number: Int) -> Rule | Nil
  text = line.strip()
  return nil if text.empty?() || text.start_with?("#")
  parts = Regexp.new("^([a-z_][a-z0-9_]*)\\s+([-ix]+)\\s+(.+)$").match(text)
  raise RulesError.new("line #{number}: expected NAME FLAGS PATTERN") if parts == nil
  options = 0
  parts[2].chars().each() do |flag|
    options = options | 1 if flag == "i"
    options = options | 4 if flag == "x"
  end
  begin
    Rule.new(parts[1], Regexp.new(parts[3], options), nil)
  rescue error: RegexpError
    raise RulesError.new("line #{number}: #{error.message()}")
  end
end

def load_rules(path: String) -> Array[Rule]
  rules = []
  number = 0
  File.open(path, "r").read().split("\n").each() do |line|
    number += 1
    rule = parse_rule_line(line, number)
    rules.push(rule) unless rule == nil
  end
  rules
end
