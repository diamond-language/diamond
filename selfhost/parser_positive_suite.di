# Batched replacement for tests/parser_diff.sh's positive-case loop: the
# old script spawned a fresh `diamond selfhost/parser_run_with_core.di`
# per case, recompiling core.di + all 5354 lines of parser.di from
# scratch every time -- by far the dominant cost of a full `make
# test-all` run (see docs/roadmap.md). parser_run_batch.di now does that
# compile exactly once per whole run; this file drives it with a single
# Process.run call, splits its one combined stdout capture back into
# per-case segments, and compares each against a fresh (still per-case,
# but cheap -- it never touches parser.di) native subprocess. Minitest
# gives every case its own pass/fail instead of the old script's "die on
# the first mismatch".
require "../lib/minitest"

def make_positive_test(case_file, native_output, self_hosted_output)
  def run_case()
    Minitest.assert_equal(native_output, self_hosted_output,
      "parser result mismatch for #{case_file}")
  end
  run_case
end

def run_positive_cases()
  case_files = ARGV
  batch_argv = []
  batch_argv.push("./build/diamond")
  batch_argv.push("selfhost/parser_run_batch.di")
  index = 0
  while index < case_files.length()
    batch_argv.push(case_files[index])
    index = index + 1
  end
  segments = Process.run(batch_argv).stdout().split(30.chr())

  suite = Minitest.new()
  index = 0
  while index < case_files.length()
    case_file = case_files[index]
    self_hosted_output = segments[index + 1]
    native_output = Process.run(["./build/diamond", case_file]).stdout()
    suite.test(case_file, make_positive_test(case_file, native_output, self_hosted_output))
    index = index + 1
  end
  suite.run!()
  puts("#{case_files.length()} parser differential cases passed")
end

run_positive_cases()
