# CLI entry point: diamond packages/drb/bin/drbc.di <input.html.drb> [output.di]
#
# Output path defaults to a plain suffix swap (trailing ".drb" -> ".di",
# e.g. "views/index.html.drb" -> "views/index.html.di") unless given
# explicitly. No exit() native exists in Diamond, so failure is signaled by
# letting compile_file's own exceptions (a bad input path, an unterminated
# tag) propagate uncaught.
require "../lib/drb"

if ARGV.length() < 1
  puts("usage: diamond packages/drb/bin/drbc.di <input.html.drb> [output.di]")
  raise "drb: missing input path"
end

input_path = ARGV[0]
if ARGV.length() >= 2
  output_path = ARGV[1]
else
  output_path = input_path.slice(0, input_path.length() - 4) + ".di"
end

Drb.compile_file(input_path, output_path)
puts("compiled #{input_path} -> #{output_path}")
