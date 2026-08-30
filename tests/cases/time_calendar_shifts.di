leap_clamp = Time.utc(2024, 3, 31, 12, 30, 45).months_ago(1)
common_clamp = Time.utc(2023, 1, 31, 12, 30, 45).months_from_now(1)
leap_year_clamp = Time.utc(2024, 2, 29, 12, 30, 45).years_from_now(1)
negative_amount = Time.utc(2024, 1, 31, 0, 0, 0).months_ago(-1)
fixed = Time.fixed("+05:30", 2024, 3, 31, 23, 59, 59).months_ago(1)
fraction = Time.parse("2024-02-29T12:00:00.125-08:00").years_from_now(1)

[
  leap_clamp.iso8601(), common_clamp.iso8601(), leap_year_clamp.iso8601(),
  negative_amount.iso8601(), fixed.iso8601(), fixed.utc_offset(), fixed.utc?(),
  fraction.iso8601(3), fraction.utc_offset(),
  Time.utc(2024, 2, 29, 0, 0, 0).years_ago(4).iso8601(),
]
