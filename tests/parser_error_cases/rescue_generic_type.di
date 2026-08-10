def attempt[T](value: T)
  begin
    raise value
  rescue error: T
    error
  end
end

42
