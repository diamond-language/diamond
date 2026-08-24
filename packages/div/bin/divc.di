# CLI entry point: diamond packages/div/bin/divc.di <input.html.div> [output.di]
#
# Output path defaults to a plain suffix swap (trailing ".div" -> ".di",
# e.g. "views/index.html.div" -> "views/index.html.di") unless given
# explicitly. A missing input path exits(1) with a usage message; a bad
# input path or an unterminated tag surfaces as compile_file's own
# uncaught exception instead (a real bug in the template, not a usage
# error, so a full backtrace is more useful than a one-line message).
require "../lib/div"

if ARGV.length() < 1
  puts("usage: diamond packages/div/bin/divc.di <input.html.div> [output.di]")
  exit(1)
end

input_path = ARGV[0]
if ARGV.length() >= 2
  output_path = ARGV[1]
else
  output_path = input_path.slice(0, input_path.length() - 4) + ".di"
end

Div.compile_file(input_path, output_path)
puts("compiled #{input_path} -> #{output_path}")
