class AppLogger
  def self.get(context)
    logger = context["logger"]
    if logger == nil
      logger = Logger.new("project_board", "debug", nil, "json")
      context["logger"] = logger
    end
    logger
  end
end

def request_log_fields(request, fields: Hash = {})
  fields.merge({"request_id": request["request_id"], "method": request["method"], "path": request["path"]})
end
def log_debug(request, context, event, fields: Hash = {}) = AppLogger.get(context).debug(event, request_log_fields(request, fields))
def log_info(request, context, event, fields: Hash = {}) = AppLogger.get(context).info(event, request_log_fields(request, fields))
def log_warn(request, context, event, fields: Hash = {}) = AppLogger.get(context).warn(event, request_log_fields(request, fields))
