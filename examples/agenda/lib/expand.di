# Turning rules into dated occurrences over a range of days. Every date is
# a UTC midnight Time; an event's clock time is added on top, then shown
# in whatever offset the caller asks for.
require "./rules"

# A class, not a struct: struct fields need a type, and Time (a native
# value) has no type name to annotate with.
class Occurrence
  attr_reader title: String
  attr_predicate all_day: Bool
  def initialize(at, title: String, all_day: Bool)
    @at = at
    @title = title
    @all_day = all_day
  end
  def at() = @at
end

def midnight(date: String) = Time.parse("#{date}T00:00:00Z")

def date_text(day) -> String = day.strftime("%Y-%m-%d")

# Does `rule` fall on the UTC day `day`?
def occurs_on?(rule: Rule, day) -> Bool
  case rule
  when Once then date_text(day) == rule.date()
  when Weekly then rule.days().include?(day.wday())
  when EveryWeeks
    start = midnight(rule.start())
    elapsed_days = ((day - start) / 86400.0).round()
    elapsed_days >= 0 && elapsed_days % (7 * rule.interval()) == 0
  when Weekdays then day.on_weekday?()
  when Monthly
    # Day 31 in a 30-day month falls on its last day instead.
    target = min(rule.day(), day.end_of_month().day())
    day.day() == target
  when LastWeekday
    day.wday() == rule.weekday() && day.days_from_now(7).month() != day.month()
  when Yearly then day.month() == rule.month() && day.day() == rule.day()
  end
end

def occurrence(rule: Rule, day) -> Occurrence
  return Occurrence.new(day, rule.title(), true) if rule.clock().empty?()
  hours = rule.clock().slice(0, 2).to_i()
  minutes = rule.clock().slice(3, 2).to_i()
  Occurrence.new(day + hours.hours() + minutes.minutes(), rule.title(), false)
end

# Every occurrence from `first` through `last` (UTC midnights), in order.
def expand(rules: Array, first, last) -> Array
  found = []
  day = first
  while day <= last
    rules.each() do |rule|
      found.push(occurrence(rule, day)) if occurs_on?(rule, day)
    end
    day = day.days_from_now(1)
  end
  found.sort_by() do |event| [event.at().to_i(), event.title()] end
end
