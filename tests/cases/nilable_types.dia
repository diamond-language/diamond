class Record
end

class ChildRecord < Record
end

def choose(flag: Bool, value: Record | Nil) -> Record | Nil
  if flag
    value
  else
    nil
  end
end

choose(true, ChildRecord.new())
