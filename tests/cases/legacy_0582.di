def test(a: Int | String, b: Int | String)
 unless a is Int || b is Int
  "neither"
 else
  "one-or-both"
 end
end
test("x", "y")
