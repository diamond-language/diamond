# Batched replacement for tests/parser_diff.sh's error-case loop, same
# reasoning as parser_positive_suite.di. No output-capture boundary is
# needed here (unlike the positive cases): a rejected parse never runs
# anything, so there's no case-program stdout to isolate -- just an error
# message string, which parser.di's own Parser#error_message already
# hands back directly. That means selfhost/parser.di is required once by
# this process and every case's self-hosted side runs in-process; only
# the native reference side spawns a (cheap, parser.di-free) subprocess.
require "parser"
require "../lib/minitest"

# Some rejections (e.g. tests/parser_error_cases/module_function_duplicate.di,
# "module singleton function is already defined") surface as the
# self-hosted compiler itself raising rather than parser.compile()
# cleanly returning false -- the native compiler's own equivalent path
# is an ordinary fail() diagnostic, but the self-hosted port doesn't
# (yet) mirror that for every rejection. The old per-process script
# tolerated this by accident: it grepped a crashed subprocess's
# combined stdout+stderr for the expected substring regardless of *why*
# the process exited nonzero. This rescue reproduces that same
# tolerance deliberately -- any failure mode's message is a valid
# self-hosted diagnostic to compare against the expected substring, not
# just a clean parser.compile() == false.
def check_message(path)
  begin
    source = File.open(path, "r").read()
    builder = ProgramBuilder.new()
    source = builder.expand_source(path, source)
    parser = Parser.new(source, builder)
    if parser.compile()
      nil
    else
      parser.error_message()
    end
  rescue error: StandardError
    error.message()
  end
end

def make_error_test(case_file, expected, native_result, self_hosted_message)
  def run_case()
    Minitest.assert(!native_result.success?(),
      "native compiler accepted parser error case #{case_file}")
    Minitest.assert(native_result.stderr().include?(expected),
      "native compiler error mismatch for #{case_file}")
    Minitest.assert(self_hosted_message != nil,
      "self-hosted parser accepted error case #{case_file}")
    Minitest.assert(self_hosted_message.include?(expected),
      "self-hosted parser error mismatch for #{case_file}")
  end
  run_case
end

def run_error_cases()
  case_files = ARGV
  suite = Minitest.new()
  index = 0
  while index < case_files.length()
    case_file = case_files[index]
    expected_path = case_file.slice(0, case_file.length() - 3) + ".err"
    expected = File.open(expected_path, "r").read().chomp()
    native_result = Process.run(["./build/diamond", case_file])
    self_hosted_message = check_message(case_file)
    suite.test(case_file,
      make_error_test(case_file, expected, native_result, self_hosted_message))
    index = index + 1
  end
  suite.run!()
  puts("#{case_files.length()} parser error differential cases passed")
end

run_error_cases()
