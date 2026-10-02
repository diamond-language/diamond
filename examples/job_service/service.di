# job_service: an HTTP API for a small durable job queue.
#
#   GET  /health            liveness check
#   POST /jobs              enqueue a job, answers 202 {"id": N}
#   GET  /jobs/N            status of job N
#   POST /jobs/N/cancel     request cancellation of job N
#
# Two threads of control share one SQLite file: this file's HTTP handler,
# and a supervised background worker (worker.di) that actually runs jobs.
# Each opens its OWN connection to the database; they never share a handle.
# A single cancellation token ties shutdown together: SIGTERM/SIGINT cancels
# it, which makes the HTTP server drain and the worker stop.
require_cut "cancellation"
require_cut "jobs"
require_cut "gremlin"
require "./store"
require "./worker"

# Request handling lives on the class as singleton methods because the HTTP
# server (gremlin) is handed `JobService.handle` as a plain function value.
# That leaves nowhere to keep the database handle but a class variable
# (`@@db`), set once at startup by `boot`.
class JobService
  def self.boot(path)
    @@db = JobStore.open(path)
  end

  def self.close()
    @@db.close()
  end

  # Builds the [status, headers, body] triple the HTTP server expects,
  # with `value` serialized as JSON.
  def self.reply(status, value)
    [status, {"Content-Type": "application/json"}, JSON.stringify(value)]
  end

  # Reads an optional integer field from the request body. A missing (or
  # null) field takes `fallback`; a non-integer or an out-of-range value
  # raises ArgumentError, which `handle` turns into a 400.
  def self.number(data, key, fallback, minimum, maximum)
    value = data[key]
    value = fallback if value == nil

    unless value is Int
      raise ArgumentError.new("#{key} must be an integer")
    end

    if value < minimum || value > maximum
      raise ArgumentError.new("#{key} out of range")
    end

    value
  end

  # The one entry point for every request: a hand-written router.
  def self.handle(request, context)
    path = request["path"]
    method = request["method"]

    # Liveness probe.
    if path == "/health" && method == "GET"
      return JobService.reply(200, {"status": "ok"})
    end

    # Enqueue. Every field is validated and clamped before anything touches
    # the database, so a bad request can never leave a half-made job.
    if path == "/jobs" && method == "POST"
      begin
        data = JSON.parse(request["body"])
        unless data is Hash then raise ArgumentError.new("expected a JSON object") end

        # `args` becomes the job's payload (the worker reads it back).
        # work_ms: how long the fake job "works"; timeout_ms: its deadline;
        # fail_until: make the first N attempts fail, to demo retries.
        args = {"work_ms": JobService.number(data, "work_ms", 100, 0, 60000),
          "timeout_ms": JobService.number(data, "timeout_ms", 5000, 0, 60000),
          "fail_until": JobService.number(data, "fail_until", 0, 0, 10)}

        # max_attempts is a property of the job row, not of the payload.
        attempts = JobService.number(data, "max_attempts", 3, 1, 10)
        id = Jobs.enqueue(@@db, kind: "count", args: args, max_attempts: attempts)
        return JobService.reply(202, {"id": id})
      rescue error: ArgumentError | JSONError
        return JobService.reply(400, {"error": error.message()})
      end
    end

    # Per-job routes: /jobs/N and /jobs/N/cancel. Splitting "/jobs/7/cancel"
    # gives ["", "jobs", "7", "cancel"], hence the indexes below.
    parts = path.split("/")
    if parts.length() >= 3 && parts[1] == "jobs"
      # The id must round-trip through Int exactly. That rejects "7abc",
      # "07", "-1" and "0", all of which should be 404, not job 7 or
      # an error.
      id = parts[2].to_i()
      if id > 0 && id.to_s() == parts[2]
        if parts.length() == 4 && parts[3] == "cancel" && method == "POST"
          # Flag the job as cancel-requested. A job that has not started
          # ('pending') is cancelled outright; a 'running' one only gets
          # the flag, and the worker notices it at its next checkpoint.
          # Finished jobs are untouched (the status IN (...) guard).
          @@db.execute("UPDATE jobs SET cancel_requested = 1, status = CASE WHEN status = 'pending' THEN 'cancelled' ELSE status END WHERE id = ? AND status IN ('pending', 'running')", [id])
        elsif !(parts.length() == 3 && method == "GET")
          return JobService.reply(404, {"error": "not found"})
        end

        # Both GET and the cancel POST answer with the job's current state.
        rows = @@db.query("SELECT id, status, attempts, max_attempts, last_error, result FROM jobs WHERE id = ?", [id])
        if rows.length() > 0 then return JobService.reply(200, rows[0]) end
      end
    end

    # Anything that fell through (unknown path, unknown job, wrong verb).
    JobService.reply(404, {"error": "not found"})
  end
end

def main(argv)
  if argv.length() != 2
    warn("usage: service.di PORT DATABASE")
    return 64
  end

  port = argv[0].to_i()
  if port < 1 || port > 65535 then raise ArgumentError.new("invalid port") end

  # Open this thread's DB connection; the worker opens its own.
  JobService.boot(argv[1])

  # One shared shutdown signal, plus a supervisor that owns the worker.
  source = Cancellation::Source.new()
  supervisor = Supervisor.new()

  # Turn SIGTERM/SIGINT into a cancellation. The handler is a nested `def`
  # that captures `source`. The handler only flips the flag; the actual
  # teardown happens in the `ensure` below.
  def stop_service()
    source.cancel()
  end
  Signal.trap("TERM", stop_service)
  Signal.trap("INT", stop_service)

  begin
    # Start the background worker. It gets its own DB path (to open a
    # separate connection) and the shared token (to know when to stop).
    supervisor.add_child(job_service_worker, argv[1], source.token())

    # Tight request limits: this is a demo, and small caps keep a hostile
    # or buggy client from tying up the server.
    limits = {"line_bytes": 2048, "header_bytes": 8192, "header_count": 32,
      "body_bytes": 4096, "connections": 16, "timeout_seconds": 2}

    # Blocks serving requests until the token is cancelled, then drains
    # in-flight connections for up to the last argument's 0.5 seconds.
    puts("job service starting on port #{port}")
    gremlin_serve(port, JobService.handle, 1, nil, nil, limits, true, source.token(), 0.5)
  ensure
    # Shut down in dependency order, and do it even if the server raised:
    # cancel (so the worker stops), wait for the worker, and only then
    # close the DB it was using.
    source.cancel()
    supervisor.stop()
    JobService.close()
  end

  puts("job service stopped")
  0
end
exit(main(ARGV))
