# Fiber resume/yield round trips in a loop -- exercises the
# ucontext-based stackful coroutine switch (diamond_fiber_resume/
# diamond_fiber_run), likely one of the most expensive per-operation
# costs in this VM and directly relevant to any future JIT scope
# decision about whether/how to specialize across a fiber boundary.
def make_pingpong()
  def loop_body()
    total = 0
    loop do
      total = total + yield(total)
    end
  end
  loop_body
end

def run()
  fiber = Fiber.new(make_pingpong())
  value = fiber.resume(0)
  index = 0
  while index < 500000
    value = fiber.resume(1)
    index = index + 1
  end
  value
end
run()
