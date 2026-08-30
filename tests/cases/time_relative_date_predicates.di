local_now = Time.now()
local_yesterday = local_now.beginning_of_day() - 1
local_tomorrow = local_now.end_of_day() + 1
east_now = Time.now().localtime("+14:00")
east_yesterday = east_now.beginning_of_day() - 1
east_tomorrow = east_now.end_of_day() + 1

east = Time.fixed("+14:00", 2024, 1, 2, 0, 0, 0)
same_projected_day = Time.utc(2024, 1, 1, 23, 0, 0)
previous_projected_day = Time.utc(2024, 1, 1, 9, 0, 0)

[
  local_yesterday.yesterday?(), local_yesterday.today?(),
  local_tomorrow.tomorrow?(), local_tomorrow.today?(),
  east_yesterday.yesterday?(), east_tomorrow.tomorrow?(),
  east.same_day?(same_projected_day), east.same_day?(previous_projected_day),
  east.same_day?(east.utc()),
]
