utc = Time.parse("1970-01-01T00:00:00Z")
east = Time.parse("1970-01-01T05:30:00+05:30")
west = Time.parse("1969-12-31T16:00:00-08:00")
fraction = Time.parse("1970-01-01T00:00:00.125Z")
leap_day = Time.parse("2024-02-29T23:59:59+14:00")

[
  utc.to_i(), utc.utc?(), utc.utc_offset(),
  east.to_i(), east.utc?(), east.utc_offset(), east.strftime("%H:%M %z"),
  west.to_i(), west.utc_offset(), west.strftime("%Y-%m-%d %H:%M %z"),
  fraction.to_f(), leap_day.strftime("%Y-%m-%d %H:%M:%S %z"),
  utc == east, east == west,
]
