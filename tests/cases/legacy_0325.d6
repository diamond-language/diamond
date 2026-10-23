def make()
 def bad()
  yield(1)
  raise RuntimeError.new("boom")
 end
 bad
end
f = Fiber.new(make())
f.resume(0)
begin
 f.resume(0)
rescue error: RuntimeError
 error.message()
end
