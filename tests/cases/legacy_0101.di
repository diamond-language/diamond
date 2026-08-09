class Box
  def set(value)
    @value = value
  end
  def value()
    @value
  end
end

box = Box.new()
normal = begin
  20
ensure
  box.set(1)
end

raised = begin
  begin
    raise "boom"
  ensure
    box.set(2)
  end
rescue error: String
  20
end

def returned(box)
  begin
    return 2
  ensure
    box.set(20)
  end
end

normal + raised + returned(box) + box.value() - 20
