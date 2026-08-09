def test(a: Int | String, b: Int | String, c: Int | String)
 if (a is Int && b is Int) || c is Int
  if c is Int
   c + 1
  else
   a + b
  end
 else
  -1
 end
end
test("x", "y", 9)
