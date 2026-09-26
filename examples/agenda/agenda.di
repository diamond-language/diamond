# agenda: expand recurring events into a dated agenda or a month calendar.
#
#   diamond agenda.di [--today YYYY-MM-DD] [--days N] [--offset +HH:MM] [--cal] FILE
#
# Lists every event from today through the next N days (default 7), grouped
# by day. --offset shows times in a fixed UTC offset (a local day can then
# start at a different UTC moment). --cal prints this month's calendar
# instead, starring days with events. --today pins "today" (UTC) so the
# output is reproducible; it defaults to the current UTC date.
require "./lib/calendar"

def usage() -> Int
  warn("usage: agenda [--today YYYY-MM-DD] [--days N] [--offset +HH:MM] [--cal] FILE")
  64
end

def read_rules(path: String) -> Array
  file = File.open(path, "r")
  text = ""
  begin
    text = file.read()
  ensure
    file.close()
  end
  rules = []
  number = 0
  text.split("\n").each() do |raw|
    number += 1
    line = raw.strip()
    next if line.empty?() || line.start_with?("#")
    begin
      rules.push(parse_rule(line))
    rescue error: RuleError | ArgumentError
      raise RuleError.new("line #{number}: #{error.message()}")
    end
  end
  rules
end

def print_agenda(events: Array, offset: String)
  current = ""
  events.each() do |event|
    shown = event.at().localtime(offset)
    heading = shown.strftime("%a %d %b %Y")
    if heading != current
      puts("") unless current.empty?()
      puts(heading)
      current = heading
    end
    time = if event.all_day?() then "all day" else shown.strftime("%H:%M") end
    puts("  #{time.ljust(8, " ")}#{event.title()}")
  end
  puts("(nothing scheduled)") if events.empty?()
end

def main(args: Array[String]) -> Int
  today = Time.utc_now().beginning_of_day()
  days = 7
  offset = "Z"
  calendar = false
  files = []
  index = 0
  while index < args.length()
    arg = args[index]
    case arg
    when "--today", "--days", "--offset"
      return usage() if index + 1 >= args.length()
      value = args[index + 1]
      begin
        case arg
        when "--today" then today = midnight(value)
        when "--days" then days = value.to_i()
        else Time.utc_now().localtime(value)   # validates the offset
        end
      rescue error: ArgumentError
        warn("agenda: #{arg} #{value}: #{error.message()}")
        return 64
      end
      offset = value if arg == "--offset"
      index += 1
    when "--cal" then calendar = true
    else
      return usage() if arg.start_with?("-")
      files.push(arg)
    end
    index += 1
  end
  return usage() if files.length() != 1 || days < 1

  rules = []
  begin
    rules = read_rules(files[0])
  rescue error: IOError
    warn("agenda: #{error.message()}")
    return 66
  rescue error: RuleError
    warn("agenda: #{files[0]}: #{error.message()}")
    return 65
  end

  if calendar
    events = expand(rules, today.beginning_of_month(), today.end_of_month().beginning_of_day())
    busy = {}
    events.each() do |event| busy[date_text(event.at())] = true end
    month_grid(today, busy).each() do |line| puts(line) end
  else
    print_agenda(expand(rules, today, today.days_from_now(days - 1)), offset)
  end
  0
end

exit(main(ARGV))
