def test(x: Int | String, y: Int | String)
 if x is Int && y is Int
  x + y
 else
  0
 end
end
test(5, "b")
