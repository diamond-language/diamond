def pheint_logging_middleware(request, context, forward)
  request["request_id"] = SecureRandom.hex(8)
  context["log_context"] = pheint_log_fields(request)
  started_at = Time.monotonic()
  pheint_log_info(request, context, "request.started")
  begin
    response = forward(request, context)
    duration_ms = (Time.monotonic() - started_at) * 1000
    pheint_log_info(request, context, "request.completed", {
      "status": response[0],
      "duration_ms": duration_ms
    })
    context["log_context"] = nil
    response
  rescue error: StandardError
    duration_ms = (Time.monotonic() - started_at) * 1000
    PheintLogger.get(context).error("request.failed", pheint_log_fields(request, {
      "error_class": "#{error}",
      "error_message": error.message(),
      "duration_ms": duration_ms
    }))
    raise error
  end
end

def app(request, context)
  ensure_pheint_models_configured(context)
  chain = rack_compose([pheint_logging_middleware], pheint_route)
  rack_run_chain(chain, 0, request, context)
end
