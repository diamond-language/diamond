saturday = Time.utc(2024, 5, 18, 12, 0, 0)
sunday = Time.utc(2024, 5, 19, 12, 0, 0)
monday = Time.utc(2024, 5, 20, 12, 0, 0)

[
  Time.now().today?(), Time.utc_now().today?(),
  Time.now().localtime("+14:00").today?(),
  Time.now().localtime("-12:00").today?(),
  Time.at(0).today?(), Time.at(0).past?(), Time.at(0).future?(),
  Time.utc(9999, 1, 1, 0, 0, 0).past?(),
  Time.utc(9999, 1, 1, 0, 0, 0).future?(),
  saturday.on_weekend?(), saturday.on_weekday?(),
  sunday.on_weekend?(), sunday.on_weekday?(),
  monday.on_weekend?(), monday.on_weekday?(),
]
