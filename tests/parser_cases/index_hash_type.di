def lookup(values: Hash[String, Int]) -> Int
  value = values["answer"]
  if value == nil
    return 0
  else
    return value
  end
end

lookup({"answer": 42})
