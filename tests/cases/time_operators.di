[
  (Time.at(0) + 3600).to_i(),
  (Time.at(3600) - 3600).to_i(),
  Time.at(3600) - Time.at(0),
  Time.at(0) < Time.at(1),
  Time.at(1) < Time.at(0),
  Time.at(0) <= Time.at(0),
  Time.at(1) > Time.at(0),
  Time.at(0) >= Time.at(0),
  Time.at(0) == Time.at(0),
  Time.at(0) == Time.at(1),
  Time.at(0) != Time.at(1),
  Time.at(0).utc() == Time.at(0),
]
