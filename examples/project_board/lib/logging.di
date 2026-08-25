class AppLogger
  def self.get(context)
    logger = context["logger"]
    if logger == nil
      logger = Logger.new("project_board", "debug")
      context["logger"] = logger
    end
    logger
  end
end

def request_label(request) = "request_id=#{request["request_id"]} method=#{request["method"]} path=#{request["path"]}"
def log_debug(request, context, message) = AppLogger.get(context).debug("#{request_label(request)} #{message}")
def log_info(request, context, message) = AppLogger.get(context).info("#{request_label(request)} #{message}")
def log_warn(request, context, message) = AppLogger.get(context).warn("#{request_label(request)} #{message}")
