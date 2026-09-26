# redact: scrub secrets and personal data out of text before sharing it.
#
#   diamond redact.di [--mask] [--rules FILE] [--summary] [FILE...]
#
# Reads each FILE in order, or stdin when there are none (or for "-"), and
# writes the redacted text to stdout. --summary prints what was replaced
# to stderr. Exit status: 0 on success, 64 for a usage error, 65 for a bad
# rules file, 66 when a file can't be opened.
require "./lib/rules"
require "./lib/tagger"

def usage() = "usage: redact [--mask] [--rules FILE] [--summary] [FILE...]"

def redact_stream(file, tagger: Tagger, rules: Array[Rule])
  loop do
    line = if file == nil then gets() else file.gets() end
    break if line == nil
    puts(tagger.redact(line, rules))
  end
end

def print_summary(counts: Hash[String, Int])
  if counts.empty?()
    warn("redact: nothing found")
    return
  end
  counts.keys().sort().each() do |name|
    warn("#{name.ljust(12, " ")}#{counts[name].to_s().rjust(4, " ")}")
  end
end

def main(args: Array[String]) -> Int
  mask = false
  summary = false
  rules = builtin_rules()
  paths = []
  index = 0
  while index < args.length()
    arg = args[index]
    if arg == "--mask"
      mask = true
    elsif arg == "--summary"
      summary = true
    elsif arg == "--rules"
      if index + 1 >= args.length()
        warn("redact: --rules needs a file\n#{usage()}")
        return 64
      end
      index += 1
      begin
        rules = rules + load_rules(args[index])
      rescue error: RulesError
        warn("redact: #{args[index]}: #{error.message()}")
        return 65
      rescue error: IOError
        warn("redact: #{error.message()}")
        return 66
      end
    elsif arg.start_with?("-") && arg != "-"
      warn("redact: unknown option #{arg}\n#{usage()}")
      return 64
    else
      paths.push(arg)
    end
    index += 1
  end
  paths = ["-"] if paths.empty?()

  tagger = Tagger.new(mask)
  paths.each() do |path|
    if path == "-"
      redact_stream(nil, tagger, rules)
    else
      begin
        file = File.open(path, "r")
      rescue error: IOError
        warn("redact: #{error.message()}")
        return 66
      end
      redact_stream(file, tagger, rules)
      file.close()
    end
  end
  print_summary(tagger.counts()) if summary
  0
end

exit(main(ARGV))
