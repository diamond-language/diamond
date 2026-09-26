# A `cal`-style month grid, weeks starting Monday. Today is bracketed and
# days with events are starred.
require "./expand"

def month_grid(today: Time, busy: Hash) -> Array[String]
  first = today.beginning_of_month()
  last = today.end_of_month()
  title = first.strftime("%B %Y")
  lines = [title.rjust(14 + title.length() / 2, " "), " Mo  Tu  We  Th  Fr  Sa  Su"]
  # Monday = 0 ... Sunday = 6
  offset = (first.wday() + 6) % 7
  cells = []
  offset.times() do |_| cells.push("    ") end
  day = first
  while day <= last
    number = day.day().to_s().rjust(2, " ")
    mark = if busy.include_key?(date_text(day)) then "*" else " " end
    cell = if day.same_day?(today) then "[#{number}]" else " #{number}#{mark}" end
    cells.push(cell)
    day = day.days_from_now(1)
  end
  cells.each_slice(7).each() do |week| lines.push(week.join("").rstrip()) end
  lines
end
