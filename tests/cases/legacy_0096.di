def fail(message)
  raise message
end

result = begin
  begin
    fail("diamond")
  rescue first
    raise first + " rescued"
  end
rescue final
  final + "!"
end

result
