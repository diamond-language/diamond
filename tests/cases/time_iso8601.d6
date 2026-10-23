utc = Time.parse("2026-08-29T12:30:45.125Z")
east = Time.parse("2026-08-29T18:00:45.125+05:30")
seconds_offset = Time.fixed(19801, 2026, 8, 29, 18, 0, 46)
negative = Time.parse("1969-12-31T23:59:59.5Z")

[
  utc.iso8601(), utc.iso8601(3), utc.iso8601(6),
  east.iso8601(), east.iso8601(3),
  seconds_offset.iso8601(),
  Time.parse(seconds_offset.iso8601()) == seconds_offset,
  negative.iso8601(3), Time.parse(negative.iso8601(3)) == negative,
]
