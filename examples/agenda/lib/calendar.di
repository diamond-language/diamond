# A `cal`-style month grid, weeks starting Monday. Today is bracketed and
# days with events are starred.
require "./expand"

# `busy` is a Hash used as a set of "YYYY-MM-DD" strings. Returns the lines
# to print.
def month_grid(today: Time, busy: Hash) -> Array[String]
  first = today.beginning_of_month()
  last = today.end_of_month()

  # Centered title, then the weekday header.
  title = first.strftime("%B %Y")
  lines = [title.rjust(14 + title.length() / 2, " "), " Mo  Tu  We  Th  Fr  Sa  Su"]

  # How many blank cells before day 1. `wday` counts from Sunday = 0, but
  # the grid starts on Monday, so shift by 6 (mod 7): Monday -> 0,
  # Sunday -> 6.
  offset = (first.wday() + 6) % 7
  cells = []
  offset.times() do |_| cells.push("    ") end

  # One 4-character cell per day: " 5*" starred, "[ 5]" for today (the
  # brackets take the place of the star's column).
  day = first
  while day <= last
    number = day.day().to_s().rjust(2, " ")
    mark = if busy.include_key?(date_text(day)) then "*" else " " end
    cell = if day.same_day?(today) then "[#{number}]" else " #{number}#{mark}" end
    cells.push(cell)
    day = day.days_from_now(1)
  end

  # Cut the flat cell list into weeks of 7, and trim each line's trailing
  # padding.
  cells.each_slice(7).each() do |week| lines.push(week.join("").rstrip()) end
  lines
end
