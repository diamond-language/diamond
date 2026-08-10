def answer() -> Int
  value = {"answer": 42}["answer"]
  if value == nil
    0
  else
    value
  end
end

answer()
