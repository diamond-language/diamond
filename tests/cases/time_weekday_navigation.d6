friday = Time.parse("2024-05-17T12:30:45.125+05:30")
saturday = Time.parse("2024-05-18T12:30:45.125+05:30")
sunday = Time.parse("2024-05-19T12:30:45.125+05:30")
monday = Time.parse("2024-05-20T12:30:45.125+05:30")
wednesday = Time.parse("2024-05-22T12:30:45.125+05:30")

[
  friday.next_weekday().iso8601(3), saturday.next_weekday().iso8601(3),
  sunday.next_weekday().iso8601(3), monday.previous_weekday().iso8601(3),
  saturday.previous_weekday().iso8601(3), sunday.previous_weekday().iso8601(3),
  wednesday.next_weekday().iso8601(3), wednesday.previous_weekday().iso8601(3),
  friday.next_weekday().utc_offset(), friday.next_weekday().utc?(),
  friday.next_weekday(5).iso8601(3), friday.next_weekday(6).iso8601(3),
  monday.previous_weekday(5).iso8601(3), saturday.next_weekday(5).iso8601(3),
  friday.next_weekday(0).to_f() == friday.to_f(),
  Time.utc(2024, 5, 20, 12, 0, 0).next_weekday(1000).wday(),
  Time.utc(2024, 5, 20, 12, 0, 0).next_weekday(1000) - Time.utc(2024, 5, 20, 12, 0, 0),
]
