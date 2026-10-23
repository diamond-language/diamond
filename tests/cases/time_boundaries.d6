utc = Time.parse("2024-02-15T12:34:56.125Z")
fixed = Time.parse("2024-02-15T12:34:56.125+05:30")
local = Time.local(2024, 2, 15, 12, 34, 56)

[
  utc.beginning_of_day().iso8601(6), utc.end_of_day().iso8601(6),
  utc.beginning_of_month().iso8601(6), utc.end_of_month().iso8601(6),
  fixed.beginning_of_day().iso8601(6), fixed.end_of_day().iso8601(6),
  fixed.beginning_of_month().iso8601(6), fixed.end_of_month().iso8601(6),
  fixed.end_of_month().utc_offset(), fixed.end_of_month().utc?(),
  utc.beginning_of_day() <= utc, utc <= utc.end_of_day(),
  local.beginning_of_day().hour(), local.beginning_of_day().min(),
  local.beginning_of_month().day(), local.beginning_of_month().utc?(),
]
