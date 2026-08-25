class PheintLogger
  def self.build(level = nil)
    selected = if level == nil then PheintEnvironment.log_level() else level end
    Logger.new("pheint", selected, nil, "json")
  end

  def self.get(context)
    logger = context["logger"]
    if logger == nil
      logger = PheintLogger.build(context["log_level"])
      context["logger"] = logger
    end
    logger
  end
end

def pheint_log_fields(request, fields: Hash = {})
  fields.merge({
    "request_id": request["request_id"],
    "method": request["method"],
    "path": request["path"],
    "environment": PheintEnvironment.name()
  })
end

def pheint_log_info(request, context, event, fields: Hash = {})
  PheintLogger.get(context).info(event, pheint_log_fields(request, fields))
end

def pheint_audit_info(context, event, fields: Hash = {})
  correlation = context["log_context"]
  correlation = {} if correlation == nil
  PheintLogger.get(context).info(event, correlation.merge(fields))
end
