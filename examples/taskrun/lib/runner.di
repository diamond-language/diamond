# Running a planned list of tasks, up to `limit` at a time. Each task's
# commands run one after another through `sh -c`, as child processes
# started with Process.spawn. The runner polls every running child's
# stdout and stderr with IO.poll, so a chatty child never blocks on a full
# pipe, and prints each task's output in one piece when the task ends.
require "./taskfile"

class Job
  attr_reader task: Task
  attr_reader exit_code: Int

  def initialize(task: Task)
    @task = task
    @next_command = 0
    @output = StringBuilder.new()
    @handle = nil
    @exit_code = 0
    @started = Time.monotonic()
    @seconds = 0.0
  end

  def seconds() -> Float = @seconds

  # Starts the next command; false once there are none left.
  def start_next() -> Bool
    return false if @next_command >= @task.commands().length()
    command = @task.commands()[@next_command]
    @next_command += 1
    @output.append("$ #{command}\n")
    @handle = Process.spawn(["sh", "-c", command])
    true
  end

  def streams() -> Array
    return [] if @handle == nil
    [@handle.stdout(), @handle.stderr()]
  end

  # Moves whatever output is ready into the buffer without blocking.
  def drain()
    self.streams().each() do |stream|
      loop do
        chunk = nil
        begin
          chunk = stream.read(4096)
        rescue error: WouldBlockError
          break
        end
        break if chunk == nil
        @output.append(chunk)
      end
    end
  end

  # :running, :failed, or :ok. Advances to the next command when the
  # current one succeeds.
  def advance() -> Symbol
    self.drain()
    return :running if @handle != nil && @handle.running?()
    if @handle != nil
      self.drain()
      @exit_code = @handle.wait()
      @handle = nil
      if @exit_code != 0
        @seconds = Time.monotonic() - @started
        return :failed
      end
    end
    return :running if self.start_next()
    @seconds = Time.monotonic() - @started
    :ok
  end

  def report(outcome: Symbol, show_time: Bool) -> String
    status = if outcome == :ok then "ok" else "FAILED (exit #{@exit_code})" end
    timing = if show_time then " #{"%.2f".format(@seconds)}s" else "" end
    text = @output.to_s()
    body = text.split("\n").reject() do |line| line.empty?() end.map() do |line|
      "  #{line}"
    end
    [ "== #{@task.name()}: #{status}#{timing}", *body ].join("\n")
  end
end

class Runner
  def initialize(tasks: Hash, limit: Int, show_time: Bool)
    @tasks = tasks
    @limit = limit
    @show_time = show_time
  end

  # Returns [ok, failed, skipped] task counts.
  def run(order: Array[String]) -> Array
    pending = order.dup()
    finished = {}
    running = []
    ok = 0
    failed = 0
    loop do
      # Start whatever is ready, unless something has already failed.
      while failed == 0 && running.length() < @limit
        ready = pending.find() do |name|
          @tasks[name].deps().all?() do |dep| finished[dep] == :ok end
        end
        break if ready == nil
        pending.delete_at(pending.index_of(ready))
        running.push(Job.new(@tasks[ready]))
      end
      break if running.empty?()

      streams = running.flat_map() do |job| job.streams() end
      IO.poll(streams, [], 20) unless streams.empty?()

      still_running = []
      running.each() do |job|
        outcome = job.advance()
        if outcome == :running
          still_running.push(job)
        else
          finished[job.task().name()] = outcome
          puts(job.report(outcome, @show_time))
          if outcome == :ok then ok += 1 else failed += 1 end
        end
      end
      running = still_running
    end
    pending.each() do |name| puts("== #{name}: skipped") end
    [ok, failed, pending.length()]
  end
end
