# Time.now()/Time.utc_now() return real wall-clock time -- can only be
# checked loosely (no exact value assertions), unlike Time.at(0)'s
# fully deterministic sibling tests.
[
  Time.now().year() >= 2024,
  Time.now().utc?(),
  Time.utc_now().utc?(),
  Time.now().to_f() > 0,
  (Time.now() - Time.now()) < 1.0,
]
