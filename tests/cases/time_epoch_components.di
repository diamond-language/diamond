t = Time.at(0).utc()

[
  t.year(), t.month(), t.day(), t.hour(), t.min(), t.sec(), t.wday(),
  t.strftime("%Y-%m-%d %H:%M:%S"),
  t.to_i(), t.to_f(),
  t.utc?(), t.localtime().utc?(), t.localtime().utc().utc?(),
]
