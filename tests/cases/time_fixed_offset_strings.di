epoch = Time.at(0)
[
  epoch.localtime("Z").strftime("%Y-%m-%d %H:%M:%S %z"),
  epoch.localtime("Z").utc_offset(), epoch.localtime("Z").utc?(),
  epoch.localtime("+05:30").strftime("%Y-%m-%d %H:%M:%S %z"),
  epoch.localtime("+05:30").utc_offset(),
  epoch.localtime("-08:00").strftime("%Y-%m-%d %H:%M:%S %z"),
  epoch.localtime("-08:00").utc_offset(),
  epoch.localtime("+23:59").utc_offset(),
  epoch.localtime("-23:59").utc_offset(),
]
