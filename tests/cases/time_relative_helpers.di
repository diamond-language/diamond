before = Time.now()
past_int = 2.ago()
past_float = 1.5.ago()
future_int = 2.from_now()
future_float = 1.5.from_now()
after = Time.now()

[
  past_int < before, past_float < before,
  future_int > after, future_float > after,
  past_int.utc?(), future_int.utc?(),
  (before - past_int) >= 1.0, (future_int - after) >= 1.0,
]
