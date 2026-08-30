utc = Time.utc(1970, 1, 1, 0, 0, 0)
east = Time.fixed("+05:30", 1970, 1, 1, 5, 30, 0)
west = Time.fixed(-28800, 1969, 12, 31, 16, 0, 0)
leap = Time.utc(2024, 2, 29, 23, 59, 59)
zero_fixed = Time.fixed("Z", 1970, 1, 1, 0, 0, 0)

[
  utc.to_i(), utc.utc?(), utc.utc_offset(),
  east.to_i(), east.utc?(), east.utc_offset(), east.strftime("%H:%M %z"),
  west.to_i(), west.utc_offset(), west.strftime("%Y-%m-%d %H:%M %z"),
  leap.strftime("%Y-%m-%d %H:%M:%S UTC"),
  zero_fixed.to_i(), zero_fixed.utc?(), zero_fixed.utc_offset(),
  utc == east, east == west,
]
