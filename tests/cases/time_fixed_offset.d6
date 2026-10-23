t = Time.at(0).localtime(19800)
west = Time.at(0).localtime(-18000)

[
  t.year(), t.month(), t.day(), t.hour(), t.min(), t.sec(),
  t.strftime("%Y-%m-%d %H:%M:%S %z"), t.to_s(), t.utc_offset(), t.utc?(),
  west.strftime("%Y-%m-%d %H:%M:%S %z"), west.utc_offset(),
  (t + 3600).strftime("%H:%M %z"),
  t.utc().utc_offset(), t.localtime(0).utc?(),
]
