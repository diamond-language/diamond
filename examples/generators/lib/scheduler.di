# A cooperative round-robin scheduler over a simulated clock. Each task is
# a Fiber. A task gives up the CPU by yielding a request -- [:sleep, ticks]
# or :pass (wait one tick) -- and the scheduler resumes every task whose
# wake time has come, once per tick, in the order they were spawned.
# Nothing preempts a task, so the trace is the same on every run.

# A scheduled task: a name, its fiber, and the tick at which it may next run.
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
    # @clock: the current tick. @log: the trace, one line per event, each
    # stamped with the tick it happened at.
    @tasks = []
    @clock = 0
    @log = []
  end

  # Registers a task (it first runs on the next tick). Tasks run in the order
  # they were spawned.
  def spawn(name: String, &body)
    @tasks.push(Task.new(name, Fiber.new(body)))
  end

  # Adds a timestamped line to the trace. Tasks call this too.
  def log(message: String)
    @log.push("t=#{@clock} #{message}")
  end

  # Runs until every task has finished or failed; returns the trace.
  def run() -> Array
    until @tasks.empty?()
      # Tasks whose wake time has arrived.
      ready = @tasks.select() do |task| task.wake_at() <= @clock end

      # Everyone is asleep: jump the clock straight to the earliest wake-up
      # instead of ticking through empty time.
      if ready.empty?()
        @clock = @tasks.map() do |task| task.wake_at() end.min()
        next
      end

      # Give each ready task one turn, then advance time.
      ready.each() do |task| self.step(task) end
      @clock += 1
    end

    @log
  end

  private

  # One turn for one task: resume its fiber, which runs until it yields a
  # request, finishes, or fails.
  def step(task: Task)
    request = nil

    # A task that raises is logged and removed; it must not take the
    # scheduler (or the other tasks) down with it.
    begin
      request = task.fiber().resume()
    rescue error: StandardError
      self.log("#{task.name()} failed: #{error.message()}")
      @tasks = @tasks.reject() do |other| other == task end
      return
    end

    # The fiber ended normally: what `resume` returned is its result.
    unless task.fiber().alive?()
      self.log("#{task.name()} finished -> #{request}")
      @tasks = @tasks.reject() do |other| other == task end
      return
    end

    # Otherwise the task yielded a request. Record when it should next wake.
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
