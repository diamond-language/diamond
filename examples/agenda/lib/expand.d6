# Turning rules into dated occurrences over a range of days. Every date is
# a UTC midnight Time; an event's clock time is added on top, then shown
# in whatever offset the caller asks for.
require "./rules"

struct Occurrence(at: Time, title: String, all_day: Bool)
end

# Parses "YYYY-MM-DD" into that day's UTC midnight (raises ArgumentError for
# an impossible date, which the callers rely on to validate input).
def midnight(date: String) -> Time = Time.parse("#{date}T00:00:00Z")

# The inverse: a Time's UTC date as "YYYY-MM-DD", used as a Hash key.
def date_text(day: Time) -> String = day.strftime("%Y-%m-%d")

# Does `rule` fall on the UTC day `day`?
# `case` over the sealed Rule: the compiler checks that all seven kinds are
# handled.
def occurs_on?(rule: Rule, day: Time) -> Bool
  case rule
  when Once then date_text(day) == rule.date()
  when Weekly then rule.days().include?(day.wday())
  # "Every N weeks from START": count whole days since the start date (round()
  # absorbs any sub-day remainder) and test whether that is a non-negative
  # multiple of N weeks. Dates before START never match.
  when EveryWeeks
    start = midnight(rule.start())
    elapsed_days = ((day - start) / 86400.0).round()
    elapsed_days >= 0 && elapsed_days % (7 * rule.interval()) == 0
  when Weekdays then day.on_weekday?()
  when Monthly
    # Day 31 in a 30-day month falls on its last day instead.
    target = min(rule.day(), day.end_of_month().day())
    day.day() == target
  # The last Friday (say) of a month is a Friday whose date a week later is
  # already in the NEXT month.
  when LastWeekday
    day.wday() == rule.weekday() && day.days_from_now(7).month() != day.month()
  when Yearly then day.month() == rule.month() && day.day() == rule.day()
  end
end

# Turns "rule R fires on day D" into a concrete Occurrence: all-day events sit
# at midnight; timed ones add the rule's HH:MM on top.
def occurrence(rule: Rule, day: Time) -> Occurrence
  return Occurrence.new(day, rule.title(), true) if rule.clock().empty?()

  # The clock is the string "HH:MM"; slice out the two numbers.
  hours = rule.clock().slice(0, 2).to_i()
  minutes = rule.clock().slice(3, 2).to_i()
  Occurrence.new(day + hours.hours() + minutes.minutes(), rule.title(), false)
end

# Every occurrence from `first` through `last` (UTC midnights), in order.
def expand(rules: Array, first: Time, last: Time) -> Array
  found = []
  day = first

  # Brute force: ask every rule about every day. Fine for personal-calendar
  # sizes and far simpler than computing each rule's next occurrence.
  while day <= last
    rules.each() do |rule|
      found.push(occurrence(rule, day)) if occurs_on?(rule, day)
    end
    day = day.days_from_now(1)
  end

  # Sort by time, then title, so events at the same moment come out in a
  # stable order.
  found.sort_by() do |event| [event.at().to_i(), event.title()] end
end
