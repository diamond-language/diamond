def noop()
  1
end

def error_name(options)
  begin
    Supervisor.new().restart_policy(options)
    "accepted"
  rescue error: TypeError
    "TypeError"
  end
end

late = Supervisor.new()
late.add_child(noop)
late_result = "accepted"
begin
  late.restart_policy({"max_restarts": 1})
rescue error: SupervisorError
  late_result = "SupervisorError"
end
[error_name(5), error_name({"max_restart": 1}), error_name({"max_restarts": -1}),
 error_name({"max_restarts": 101}), error_name({"within_ms": 0}),
 error_name({"backoff_ms": "fast"}), error_name({1: 2}),
 error_name({"backoff_ms": 50, "max_backoff_ms": 10}), error_name({}), late_result]
