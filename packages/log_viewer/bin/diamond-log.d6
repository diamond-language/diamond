require "../lib/log_viewer"

strict = false
path = nil
index = 0
while index < ARGV.length()
  argument = ARGV[index]
  if argument == "--strict"
    strict = true
  elsif argument == "--help"
    puts("usage: diamond-log [--strict] [log.ndjson]")
    exit(0)
  elsif path == nil
    path = argument
  else
    puts("usage: diamond-log [--strict] [log.ndjson]")
    exit(1)
  end
  index += 1
end

input = if path == nil then nil else File.open(path, "r") end
invalid = false
loop do
  line = if input == nil then gets() else input.gets() end
  if line == nil
    break
  end
  formatted = LogViewer.format_line(line)
  puts(formatted[0])
  unless formatted[1]
    invalid = true
  end
end
unless input == nil
  input.close()
end
if strict && invalid
  exit(1)
end
exit(0)
