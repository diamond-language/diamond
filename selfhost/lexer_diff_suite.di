# Batched replacement for tests/lexer_diff.sh's per-case process: the old
# script spawned a fresh `diamond selfhost/lexer_dump.di` (recompiling
# lexer.di from source) for every one of ~1050 cases, which dominated CI
# time (see docs/roadmap.md). Here selfhost/lexer.di is required exactly
# once for the whole run; dump_tokens is called directly in a loop
# instead of shelling out per case. The native reference side still uses
# a real subprocess per case (Process.run) since it has to reflect
# whatever tests/lexer_dump.c currently does, not a frozen snapshot --
# but that subprocess never touches lexer.di, so it stays cheap.
require "lexer"
require "../lib/minitest"

def dump_tokens(path)
  source = File.open(path, "r").read()
  lexer = Lexer.new(source)
  builder = StringBuilder.new()
  loop do
    t = lexer.next_token()
    builder.append("#{t.kind()} #{t.start()} #{t.length()} #{t.line()} #{t.column()}\n")
    break if t.kind() == :eof
  end
  builder.to_s()
end

# Wrapped in its own `def` (a fresh parameter binding per call) rather
# than closing over a shared while-loop variable directly: Diamond
# closures capture shared mutable storage, so every test registered
# straight from the loop body below would end up reading whatever
# case_file happened to be last once Minitest actually invokes them --
# see lib/minitest.di's own comment on why each test needs a real
# closure, and this file's run_lexer_diff for why that closure still
# needs an independent binding per case.
def make_lexer_test(case_file)
  def run_case()
    expected = Process.run(["./build/lexer_dump", case_file]).stdout()
    actual = dump_tokens(case_file)
    Minitest.assert_equal(expected, actual, "token stream mismatch for #{case_file}")
  end
  run_case
end

def run_lexer_diff()
  suite = Minitest.new()
  index = 0
  while index < ARGV.length()
    case_file = ARGV[index]
    suite.test(case_file, make_lexer_test(case_file))
    index = index + 1
  end
  suite.run!()
  puts("#{ARGV.length()} lexer differential cases passed")
end

run_lexer_diff()
