utc = Time.parse("2024-05-15T12:34:56.125Z")
fixed = Time.parse("2024-12-31T12:34:56.125-08:00")
monday = Time.utc(2024, 5, 13, 9, 0, 0)
sunday = Time.utc(2024, 5, 19, 9, 0, 0)

[
  utc.beginning_of_week().iso8601(6), utc.end_of_week().iso8601(6),
  utc.beginning_of_year().iso8601(6), utc.end_of_year().iso8601(6),
  fixed.beginning_of_week().iso8601(6), fixed.end_of_week().iso8601(6),
  fixed.beginning_of_year().iso8601(6), fixed.end_of_year().iso8601(6),
  fixed.end_of_year().utc_offset(), fixed.end_of_year().utc?(),
  monday.beginning_of_week().iso8601(), sunday.end_of_week().iso8601(6),
]
