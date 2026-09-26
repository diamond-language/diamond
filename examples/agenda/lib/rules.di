# Recurrence rules, one per line of an events file:
#
#   2026-10-03 14:00 Dentist                  one-off date
#   every mon,wed 09:30 Standup               weekly on the given days
#   every 2 weeks from 2026-09-07 15:00 Sprint review
#   weekdays 08:15 Commute                    Monday to Friday
#   monthly 1 09:00 Rent due                  a day of the month (clamped)
#   last fri 16:00 Demo day                   last given weekday of each month
#   yearly 03-14 Pi day                       a month and day, all day
#
# A time is optional; without one the event is all-day. Rule is sealed, so
# every case over a rule must handle all seven kinds.

class RuleError < StandardError
end

sealed class Rule
  attr_reader title: String
  attr_reader clock: String      # "HH:MM", or "" for all day
  def initialize(title: String, clock: String)
    @title = title
    @clock = clock
  end
end

class Once < Rule
  attr_reader date: String
  def initialize(date: String, title: String, clock: String)
    super(title, clock)
    @date = date
  end
end

class Weekly < Rule
  attr_reader days: Array[Int]   # 0 = Sunday .. 6 = Saturday
  def initialize(days: Array[Int], title: String, clock: String)
    super(title, clock)
    @days = days
  end
end

class EveryWeeks < Rule
  attr_reader interval: Int
  attr_reader start: String
  def initialize(interval: Int, start: String, title: String, clock: String)
    super(title, clock)
    @interval = interval
    @start = start
  end
end

class Weekdays < Rule
end

class Monthly < Rule
  attr_reader day: Int
  def initialize(day: Int, title: String, clock: String)
    super(title, clock)
    @day = day
  end
end

class LastWeekday < Rule
  attr_reader weekday: Int
  def initialize(weekday: Int, title: String, clock: String)
    super(title, clock)
    @weekday = weekday
  end
end

class Yearly < Rule
  attr_reader month: Int
  attr_reader day: Int
  def initialize(month: Int, day: Int, title: String, clock: String)
    super(title, clock)
    @month = month
    @day = day
  end
end

def weekday_number(name: String) -> Int
  number = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"].index_of(name.downcase())
  raise RuleError.new("unknown weekday '#{name}'") if number == nil
  number
end

# Splits "09:30 Standup" into ["09:30", "Standup"], or ["", text] when the
# text doesn't start with a time.
def split_clock(text: String) -> Array
  found = Regexp.new("^([0-2][0-9]:[0-5][0-9]) +(.+)$").match(text)
  return ["", text] if found == nil
  [_, clock, title] = found
  raise RuleError.new("bad time #{clock}") if clock.slice(0, 2).to_i() > 23
  [clock, title]
end

def parse_rule(line: String) -> Rule
  words = line.split(" ").reject() do |word| word.empty?() end
  case words
  when [date, *rest] if Regexp.new("^[0-9]{4}-[0-9]{2}-[0-9]{2}$").match?(date)
    [clock, title] = split_clock(rest.join(" "))
    Time.parse("#{date}T00:00:00Z")   # rejects impossible dates
    Once.new(date, title, clock)
  when ["every", count, "weeks", "from", start, *rest]
    [clock, title] = split_clock(rest.join(" "))
    Time.parse("#{start}T00:00:00Z")
    EveryWeeks.new(count.to_i(), start, title, clock)
  when ["every", days, *rest]
    [clock, title] = split_clock(rest.join(" "))
    Weekly.new(days.split(",").map() do |day| weekday_number(day) end, title, clock)
  when ["weekdays", *rest]
    [clock, title] = split_clock(rest.join(" "))
    Weekdays.new(title, clock)
  when ["monthly", day, *rest]
    [clock, title] = split_clock(rest.join(" "))
    Monthly.new(day.to_i(), title, clock)
  when ["last", weekday, *rest]
    [clock, title] = split_clock(rest.join(" "))
    LastWeekday.new(weekday_number(weekday), title, clock)
  when ["yearly", month_day, *rest]
    [clock, title] = split_clock(rest.join(" "))
    [month, day] = month_day.split("-").map() do |part| part.to_i() end
    Yearly.new(month, day, title, clock)
  else
    raise RuleError.new("can't read rule: #{line}")
  end
end
