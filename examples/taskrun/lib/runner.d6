# Running a planned list of tasks, up to `limit` at a time. Each task's
# commands run one after another through `sh -c`, as child processes
# started with Process.spawn. The runner polls every running child's
# stdout and stderr with IO.poll, so a chatty child never blocks on a full
# pipe, and prints each task's output in one piece when the task ends.
require "./taskfile"

# One task in progress. It runs the task's commands one at a time: `advance`
# is called repeatedly, and each call either keeps waiting, finishes the task,
# or starts the next command.
class Job
  attr_reader task: Task
  attr_reader exit_code: Int

  def initialize(task: Task)
    @task = task
    # @next_command: index of the next command to start. @output: everything
    # the task printed (held back and shown in one piece when the task ends,
    # so parallel tasks do not interleave). @handle: the running child
    # process, or nil between commands.
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

    # Echo the command into the output first, then run it via `sh -c` so
    # pipes, `&&` and so on work.
    @output.append("$ #{command}\n")
    @handle = Process.spawn(["sh", "-c", command])
    true
  end

  # The child's stdout and stderr, for the runner to wait on (none between
  # commands).
  def streams() -> Array
    return [] if @handle == nil
    [@handle.stdout(), @handle.stderr()]
  end

  # Moves whatever output is ready into the buffer without blocking.
  # Reading until the pipe has nothing more RIGHT NOW (WouldBlockError) keeps
  # the child from stalling on a full pipe, while never making us wait.
  def drain()
    self.streams().each() do |stream|
      loop do
        chunk = nil
        begin
          chunk = stream.read(4096)
        rescue error: WouldBlockError
          break
        end

        # nil is end of stream.
        break if chunk == nil
        @output.append(chunk)
      end
    end
  end

  # :running, :failed, or :ok. Advances to the next command when the
  # current one succeeds.
  def advance() -> Symbol
    self.drain()

    # The current command is still going: nothing to do yet.
    return :running if @handle != nil && @handle.running?()

    # The command just finished. Drain once more, since output may have
    # arrived between the check above and the exit, then collect its exit
    # code. Any non-zero code fails the whole task immediately.
    if @handle != nil
      self.drain()
      @exit_code = @handle.wait()
      @handle = nil
      if @exit_code != 0
        @seconds = Time.monotonic() - @started
        return :failed
      end
    end

    # Succeeded: start the next command, or, if that was the last, the task
    # is done.
    return :running if self.start_next()
    @seconds = Time.monotonic() - @started
    :ok
  end

  # The task's block of output: a header line, then its captured output
  # indented, blank lines dropped.
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

# The scheduler: keeps up to `limit` Jobs going at once.
class Runner
  def initialize(tasks: Hash, limit: Int, show_time: Bool)
    @tasks = tasks
    @limit = limit
    @show_time = show_time
  end

  # Returns [ok, failed, skipped] task counts.
  def run(order: Array[String]) -> Array
    # pending: planned but not started, in dependency order. finished: name
    # -> :ok or :failed. running: Jobs in flight.
    pending = order.dup()
    finished = {}
    running = []
    ok = 0
    failed = 0

    loop do
      # Fill the free slots. A task is ready when every dependency finished
      # OK, so tasks with no dependency relation run side by side. Once one
      # task has failed, nothing new starts (the rest are reported as
      # skipped), though jobs already running are allowed to finish.
      while failed == 0 && running.length() < @limit
        ready = pending.find() do |name|
          @tasks[name].deps().all?() do |dep| finished[dep] == :ok end
        end
        break if ready == nil
        pending.delete_at(pending.index_of(ready))
        running.push(Job.new(@tasks[ready]))
      end

      # Nothing running and nothing startable: finished (or blocked by a
      # failure).
      break if running.empty?()

      # Sleep until any child has output, but at most 20 ms: a child can also
      # exit without printing anything, and that must be noticed promptly.
      streams = running.flat_map() do |job| job.streams() end
      IO.poll(streams, [], 20) unless streams.empty?()

      # Give every job a turn; keep the unfinished ones, and for each that
      # finished, record the outcome and print its report.
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

    # Whatever never started was skipped because of a failure.
    pending.each() do |name| puts("== #{name}: skipped") end
    [ok, failed, pending.length()]
  end
end
