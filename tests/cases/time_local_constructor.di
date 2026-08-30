local = Time.local(2024, 1, 15, 12, 30, 45)
shifted = local.months_from_now(1)

[
  local.year(), local.month(), local.day(), local.hour(), local.min(), local.sec(),
  local.utc?(), local.localtime().to_i() == local.to_i(),
  shifted.year(), shifted.month(), shifted.day(), shifted.hour(), shifted.utc?(),
]
