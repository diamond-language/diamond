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

# Exit codes follow sysexits: 64 usage error, 65 bad data (a rule that does
# not parse), 66 cannot open the input file.
def usage() -> Int
  warn("usage: agenda [--today YYYY-MM-DD] [--days N] [--offset +HH:MM] [--cal] FILE")
  64
end

# Reads the events file into Rule objects. Any rule that fails to parse is
# reported with its line number, so a typo in a long file is easy to find.
def read_rules(path: String) -> Array
  # Slurp the whole file; `ensure` closes it even if read() raises.
  file = File.open(path, "r")
  text = ""
  begin
    text = file.read()
  ensure
    file.close()
  end

  rules = []
  number = 0

  # One rule per line; blank lines and `#` comments are skipped (but still
  # counted, so reported line numbers match the file).
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

# Prints events grouped under one heading per day. `events` is already in
# time order, so a day's events are adjacent and it is enough to print a
# heading whenever the day changes.
def print_agenda(events: Array, offset: String)
  current = ""

  events.each() do |event|
    # Convert to the requested UTC offset BEFORE taking the date: an event
    # at 23:00 UTC is already "tomorrow" at +02:00.
    shown = event.at().localtime(offset)
    heading = shown.strftime("%a %d %b %Y")

    # New day: blank line between days (not before the first), then heading.
    if heading != current
      puts("") unless current.empty?()
      puts(heading)
      current = heading
    end

    time = if event.all_day() then "all day" else shown.strftime("%H:%M") end
    puts("  #{time.ljust(8, " ")}#{event.title()}")
  end

  puts("(nothing scheduled)") if events.empty?()
end

def main(args: Array[String]) -> Int
  # Defaults: today (UTC midnight), a 7-day window, UTC ("Z"), agenda view.
  today = Time.utc_now().beginning_of_day()
  days = 7
  offset = "Z"
  calendar = false
  files = []

  # Hand-rolled argument parsing, one pass. The three options that take a
  # value share one arm: it checks a value follows, applies it, and skips it.
  index = 0
  while index < args.length()
    arg = args[index]

    case arg
    when "--today", "--days", "--offset"
      return usage() if index + 1 >= args.length()
      value = args[index + 1]

      # Apply the option. A bad date or offset raises ArgumentError, which
      # becomes a usage error naming the offending option.
      begin
        case arg
        when "--today" then today = midnight(value)
        when "--days" then days = value.to_i()
        # --offset is validated here by trying it once; the real conversion
        # happens later, in print_agenda.
        else Time.utc_now().localtime(value)
        end
      rescue error: ArgumentError
        warn("agenda: #{arg} #{value}: #{error.message()}")
        return 64
      end
      offset = value if arg == "--offset"

      # Skip the value we just consumed.
      index += 1
    when "--cal" then calendar = true
    else
      # An unknown flag is an error; anything else is the events file.
      return usage() if arg.start_with?("-")
      files.push(arg)
    end

    index += 1
  end

  # Exactly one file, and a positive day count.
  return usage() if files.length() != 1 || days < 1

  # Load the rules, mapping each failure to its exit code.
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
    # Month view: expand the whole month, remember which dates have any
    # event (a Hash used as a set), and let month_grid star those days.
    events = expand(rules, today.beginning_of_month(), today.end_of_month().beginning_of_day())
    busy = {}
    events.each() do |event| busy[date_text(event.at())] = true end
    month_grid(today, busy).each() do |line| puts(line) end
  else
    # Agenda view: `days - 1` because the range includes both end days, so a
    # 7-day window ends 6 days after today.
    print_agenda(expand(rules, today, today.days_from_now(days - 1)), offset)
  end
  0
end

exit(main(ARGV))
