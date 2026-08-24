# CLI entry point: diamond packages/div/bin/divc.di <input.html.div> [output.di]
#
# Output path defaults to a `.cache/` subdirectory next to the input file
# (a suffix swap within it -- trailing ".div" -> ".di" -- e.g.
# "views/index.html.div" -> "views/.cache/index.html.di") unless an
# explicit output path is given as a second argument. Compiled output
# deliberately doesn't sit alongside its own .html.div source: a stray
# generated .di file is easy to mistake for something hand-written, and
# keeping every compiled file under one `.cache/` directory makes it
# obvious at a glance what's source and what's a build artifact (and
# trivial to .gitignore as a whole directory, not a scattered pattern).
# `.cache/` is created if it doesn't exist yet -- self-sufficient
# regardless of how/where this is invoked from, not dependent on a
# caller script remembering to `mkdir -p` first.
#
# A missing input path exits(1) with a usage message; a bad input path
# or an unterminated tag surfaces as compile_file's own uncaught
# exception instead (a real bug in the template, not a usage error, so a
# full backtrace is more useful than a one-line message).
require "../lib/div"

if ARGV.length() < 1
  puts("usage: diamond packages/div/bin/divc.di <input.html.div> [output.di]")
  exit(1)
end

input_path = ARGV[0]
if ARGV.length() >= 2
  output_path = ARGV[1]
else
  parts = input_path.split("/")
  basename = parts.pop()
  directory = parts.join("/")
  cache_dir = if directory == "" then ".cache" else "#{directory}/.cache" end
  Process.run(["mkdir", "-p", cache_dir])
  output_name = basename.slice(0, basename.length() - 4) + ".di"
  output_path = "#{cache_dir}/#{output_name}"
end

Div.compile_file(input_path, output_path)
puts("compiled #{input_path} -> #{output_path}")
