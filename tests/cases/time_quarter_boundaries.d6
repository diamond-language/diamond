q1 = Time.parse("2024-02-29T12:34:56.125Z")
q2 = Time.parse("2024-05-15T12:34:56.125Z")
q3 = Time.parse("2024-08-15T12:34:56.125+05:30")
q4 = Time.parse("2024-12-31T12:34:56.125-08:00")

[
  q1.beginning_of_quarter().iso8601(6), q1.end_of_quarter().iso8601(6),
  q2.beginning_of_quarter().iso8601(6), q2.end_of_quarter().iso8601(6),
  q3.beginning_of_quarter().iso8601(6), q3.end_of_quarter().iso8601(6),
  q4.beginning_of_quarter().iso8601(6), q4.end_of_quarter().iso8601(6),
  q3.end_of_quarter().utc_offset(), q3.end_of_quarter().utc?(),
]
