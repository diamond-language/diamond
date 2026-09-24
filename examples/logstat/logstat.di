# logstat: summarize newline-delimited JSON logs.
#
#   diamond logstat.di [--top N] [--strict] [FILE...]
#   diamond build logstat.di && ./logstat app.log
#
# Reads each FILE in order, or stdin when there are none (or for "-").
# Exit status: 0 on success, 1 with --strict when any line was unparsed,
# 64 for a usage error, 66 when a file can't be opened.
require "./lib/lines"
require "./lib/report"

def logstat_usage() = "usage: logstat [--top N] [--strict] [FILE...]"

# Diamond has no stderr writer for the running program yet; /dev/stderr works
# on Linux, macOS, and FreeBSD. Append mode, so redirecting stderr to a file
# shared with stdout never truncates output already written there.
def logstat_error(message)
  stream = File.open("/dev/stderr", "a")
  stream.write("logstat: #{message}\n")
  stream.close()
end

def logstat_read(file, tally, number)
  loop do
    text = if file == nil then gets() else file.gets() end
    break if text == nil
    number += 1
    if text.strip().length() > 0
      tally.record(logstat_parse(text, number))
    end
  end
  number
end

def logstat_main(args) -> Int
  limit = 5
  strict = false
  paths = []
  index = 0
  while index < args.length()
    arg = args[index]
    if arg == "--help" || arg == "-h"
      puts(logstat_usage())
      return 0
    elsif arg == "--strict"
      strict = true
    elsif arg == "--top"
      if index + 1 >= args.length() || args[index + 1].to_i() < 1
        logstat_error("--top needs a positive number")
        return 64
      end
      limit = args[index + 1].to_i()
      index += 1
    elsif arg.start_with?("-") && arg != "-"
      logstat_error("unknown option #{arg}\n#{logstat_usage()}")
      return 64
    else
      paths.push(arg)
    end
    index += 1
  end
  if paths.length() == 0 then paths = ["-"] end

  tally = Tally.new()
  number = 0
  position = 0
  while position < paths.length()
    path = paths[position]
    if path == "-"
      number = logstat_read(nil, tally, number)
    else
      file = nil
      begin
        file = File.open(path, "r")
      rescue error: IOError
        logstat_error(error.message())
        return 66
      end
      number = logstat_read(file, tally, number)
      file.close()
    end
    position += 1
  end
  print(tally.render(limit))
  if strict && tally.unparsed_count() > 0 then 1 else 0 end
end

exit(logstat_main(ARGV))
