def make_worker()
  def worker()
    first = Fiber.yield(4)
    second = Fiber.yield()
    [first, second]
  end
  worker
end

fiber = Fiber.new(make_worker())
puts(fiber.resume())
puts(fiber.resume(10))
puts(fiber.resume(20))
