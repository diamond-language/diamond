def handle(reraise)
  begin
    raise 42
  rescue error
    raise if reraise
    7
  end
end

begin
  puts(handle(false))
  puts(handle(true))
rescue outer
  puts(outer)
end
