# Companion to parser_positive_suite.di: runs every case path given on
# ARGV through the self-hosted parser (parse_and_run_with_core), once
# per process instead of once per case. Diamond has no way to capture a
# nested program's own stdout from inside the same process (builder.run()
# writes straight to this process's real stdout), so parser_positive_suite.di
# can't call parse_and_run_with_core directly and still get per-case
# output isolation -- this file is that isolation boundary, spawned
# exactly once by parser_positive_suite.di via Process.run rather than
# once per case the way the old tests/parser_diff.sh did.
#
# Each case's output is preceded by a single 0x1E (ASCII record
# separator) byte, chosen because it's not producible by ordinary `puts`/
# `print` text and cases are a curated, non-adversarial fixture corpus --
# parser_positive_suite.di splits the captured stdout back into per-case
# segments on that byte. A case that raises is caught here (rather than
# aborting the whole batch, which would silently skip every later case)
# and reported as a distinguishable, guaranteed-not-to-match segment.
require "parser"

# puts(...), not a bare call: the native reference side's own printed
# output always ends with the CLI's own auto-printed top-level result
# (see src/repl.c/src/main.c -- any `diamond program.di` run prints its
# program's final expression value), and every tests/parser_cases/*.di
# fixture deliberately ends in a bare value for exactly this reason. This
# explicit puts reproduces that same line for the self-hosted side, since
# parse_and_run_with_core's return value never otherwise reaches stdout
# here the way it does when the case runs as its own top-level program.
index = 0
while index < ARGV.length()
  path = ARGV[index]
  print(30.chr())
  begin
    puts(parse_and_run_with_core(path))
  rescue error: StandardError
    print("SELF-HOSTED-ERROR: #{error.message()}")
  end
  index = index + 1
end
# One more marker after the loop: this whole file's own top-level result
# (the while loop above) gets auto-printed too, same as every case's
# does -- without this, that trailing line would land inside the last
# case's own segment instead of a segment nothing reads.
print(30.chr())
