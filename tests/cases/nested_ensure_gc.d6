# The synthesized exception never occupied a source register. While the
# inner ensure runs, only the suspended pending-unwind state owns it.
def runtime_exception()
  begin
    begin
      1 / 0
    ensure
      begin
        3
      ensure
        100.times() do |i| ["garbage #{i}", i] end
      end
    end
  rescue error: ZeroDivisionError
    error.message()
  end
end

def suspended()
  begin
    return ["fiber result"]
  ensure
    begin
      2
    ensure
      Fiber.yield("paused")
      100.times() do |i| [i, "garbage"] end
    end
  end
end
fiber = Fiber.new(suspended)
first = fiber.resume()
100.times() do |i| ["outside #{i}"] end
[first, fiber.resume(), runtime_exception()]
