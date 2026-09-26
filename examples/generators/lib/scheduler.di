# A cooperative round-robin scheduler over a simulated clock. Each task is
# a Fiber. A task gives up the CPU by yielding a request -- [:sleep, ticks]
# or :pass (wait one tick) -- and the scheduler resumes every task whose
# wake time has come, once per tick, in the order they were spawned.
# Nothing preempts a task, so the trace is the same on every run.

class Task
  attr_reader name: String
  attr_reader fiber: Fiber
  attr_accessor wake_at: Int

  def initialize(name: String, fiber: Fiber)
    @name = name
    @fiber = fiber
    @wake_at = 0
  end
end

class Scheduler
  def initialize()
    @tasks = []
    @clock = 0
    @log = []
  end

  def spawn(name: String, &body)
    @tasks.push(Task.new(name, Fiber.new(body)))
  end

  def log(message: String)
    @log.push("t=#{@clock} #{message}")
  end

  # Runs until every task has finished or failed; returns the trace.
  def run() -> Array
    until @tasks.empty?()
      ready = @tasks.select() do |task| task.wake_at() <= @clock end
      if ready.empty?()
        @clock = @tasks.map() do |task| task.wake_at() end.min()
        next
      end
      ready.each() do |task| self.step(task) end
      @clock += 1
    end
    @log
  end

  private

  def step(task: Task)
    request = nil
    begin
      request = task.fiber().resume()
    rescue error: StandardError
      self.log("#{task.name()} failed: #{error.message()}")
      @tasks = @tasks.reject() do |other| other == task end
      return
    end
    unless task.fiber().alive?()
      self.log("#{task.name()} finished -> #{request}")
      @tasks = @tasks.reject() do |other| other == task end
      return
    end
    case request
    when [:sleep, ticks] if ticks is Int && ticks > 0
      task.wake_at = @clock + ticks
    when :pass
      task.wake_at = @clock + 1
    else
      raise ArgumentError.new("#{task.name()} yielded an unknown request: #{request}")
    end
  end
end

# Helpers for task bodies, so they read like blocking code.
def sleep_ticks(ticks: Int)
  Fiber.yield([:sleep, ticks])
end

def pass()
  Fiber.yield(:pass)
end
