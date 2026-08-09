def answer(value: Int | Nil) -> Int
 unless value == nil
  value + 0
 else
  0
 end
end
answer(42)
