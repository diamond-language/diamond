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

# Message text and a one-line stderr helper.
def logstat_usage() = "usage: logstat [--top N] [--strict] [FILE...]"

def logstat_error(message)
  warn("logstat: #{message}")
end

# Feeds every line of one input into `tally`. `file` is nil for stdin. Takes
# and returns the running line `number` so numbering continues across several
# files. Blank lines are skipped but still counted, so a line number always
# matches the file.
def logstat_read(file, tally, number)
  loop do
    text = if file == nil then gets() else file.gets() end
    break if text == nil
    number += 1
    unless text.strip().empty?()
      tally.record(logstat_parse(text, number))
    end
  end
  number
end

def logstat_main(args) -> Int
  # Defaults, then one pass over the arguments.
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

      # Skip the number we just consumed.
      index += 1
    # A lone "-" means stdin, so it is a path, not an option.
    elsif arg.start_with?("-") && arg != "-"
      logstat_error("unknown option #{arg}\n#{logstat_usage()}")
      return 64
    else
      paths.push(arg)
    end
    index += 1
  end

  # No files given: read stdin.
  paths = ["-"] if paths.empty?()

  # Read every input in order into ONE tally.
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

  # `print`, not `puts`: render's text already ends with a newline.
  print(tally.render(limit))

  # --strict turns unparsed lines into a failing exit status.
  if strict && tally.unparsed_count() > 0 then 1 else 0 end
end

exit(logstat_main(ARGV))
