utc = Time.parse("2024-02-28T12:30:45.125Z")
fixed = Time.parse("2024-12-31T23:59:59.125+05:30")

[
  utc.days_from_now(1).iso8601(3), utc.days_ago(-1).iso8601(3),
  utc.weeks_from_now(1).iso8601(3), utc.weeks_ago(1).iso8601(3),
  fixed.days_from_now(1).iso8601(3), fixed.days_from_now(1).utc_offset(),
  fixed.weeks_from_now(1).iso8601(3), fixed.weeks_ago(-1).iso8601(3),
  Time.utc(2023, 12, 31, 0, 0, 0).days_from_now(1).iso8601(),
]
