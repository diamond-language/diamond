# One JSON-line Logger per worker, created on first use and cached in
# `context` (the per-worker Hash the server passes to every request).
class AppLogger
  def self.get(context)
    logger = context["logger"]

    if logger == nil
      # A test can pin the level through context; otherwise use the
      # environment's.
      level = context["log_level"]
      if level == nil
        level = AppEnvironment.log_level()
      end
      logger = Logger.new("project_board", level, nil, "json")
      context["logger"] = logger
    end
    logger
  end
end

# Adds the request id, method and path to a log event's own fields, so that
# every line from one request can be correlated (the request id is assigned
# in logging_middleware).
def request_log_fields(request, fields: Hash = {})
  fields.merge({"request_id": request["request_id"], "method": request["method"], "path": request["path"]})
end

# One-line shorthands: log_debug/info/warn(request, context, "event.name",
# {extra fields}).
def log_debug(request, context, event, fields: Hash = {}) = AppLogger.get(context).debug(event, request_log_fields(request, fields))
def log_info(request, context, event, fields: Hash = {}) = AppLogger.get(context).info(event, request_log_fields(request, fields))
def log_warn(request, context, event, fields: Hash = {}) = AppLogger.get(context).warn(event, request_log_fields(request, fields))
