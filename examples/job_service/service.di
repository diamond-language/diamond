require_cut "cancellation"
require_cut "jobs"
require_cut "gremlin"
require "./store"
require "./worker"

class JobService
  def self.boot(path)
    @@db = JobStore.open(path)
  end
  def self.close()
    @@db.close()
  end
  def self.reply(status, value)
    [status, {"Content-Type": "application/json"}, JSON.stringify(value)]
  end
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
  def self.handle(request, context)
    path = request["path"]
    method = request["method"]
    if path == "/health" && method == "GET"
      return JobService.reply(200, {"status": "ok"})
    end
    if path == "/jobs" && method == "POST"
      begin
        data = JSON.parse(request["body"])
        unless data is Hash then raise ArgumentError.new("expected a JSON object") end
        args = {"work_ms": JobService.number(data, "work_ms", 100, 0, 60000),
          "timeout_ms": JobService.number(data, "timeout_ms", 5000, 0, 60000),
          "fail_until": JobService.number(data, "fail_until", 0, 0, 10)}
        attempts = JobService.number(data, "max_attempts", 3, 1, 10)
        id = Jobs.enqueue(@@db, kind: "count", args: args, max_attempts: attempts)
        return JobService.reply(202, {"id": id})
      rescue error: ArgumentError | JSONError
        return JobService.reply(400, {"error": error.message()})
      end
    end
    parts = path.split("/")
    if parts.length() >= 3 && parts[1] == "jobs"
      id = parts[2].to_i()
      if id > 0 && id.to_s() == parts[2]
        if parts.length() == 4 && parts[3] == "cancel" && method == "POST"
          @@db.execute("UPDATE jobs SET cancel_requested = 1, status = CASE WHEN status = 'pending' THEN 'cancelled' ELSE status END WHERE id = ? AND status IN ('pending', 'running')", [id])
        elsif !(parts.length() == 3 && method == "GET")
          return JobService.reply(404, {"error": "not found"})
        end
        rows = @@db.query("SELECT id, status, attempts, max_attempts, last_error, result FROM jobs WHERE id = ?", [id])
        if rows.length() > 0 then return JobService.reply(200, rows[0]) end
      end
    end
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
  JobService.boot(argv[1])
  source = Cancellation::Source.new()
  supervisor = Supervisor.new()
  begin
    supervisor.add_child(job_service_worker, argv[1], source.token())
    limits = {"line_bytes": 2048, "header_bytes": 8192, "header_count": 32,
      "body_bytes": 4096, "connections": 16, "timeout_seconds": 2}
    puts("job service starting on port #{port}")
    gremlin_serve(port, JobService.handle, 1, nil, nil, limits, true)
  ensure
    source.cancel()
    supervisor.stop()
    JobService.close()
  end
  puts("job service stopped")
  0
end
exit(main(ARGV))
