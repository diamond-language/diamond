# Generalizes the near-identical AppLogger/logging_middleware/
# log_debug/log_info/log_warn pattern independently duplicated across
# applications/skindicate.dia, examples/project_board, and
# applications/pheint.dia (examples/library has its own much smaller
# `puts`-based logging_middleware with no Logger involved at all --
# out of scope here). Modeled on pheint's own version, the most
# complete of the three: it additionally logs a failed request rather
# than letting an exception cross the middleware with no record of it
# at all, which skindicate's and project_board's copies didn't do.
#
# One Logger, memoized on `context` (per-worker, matching Database.get's
# own memoization pattern in every one of those apps -- see
# docs/threads.md on why: each gremlin_serve(threads: N) worker is a
# fully independent VM/heap, so this has to be looked up per-request,
# not built once at startup and captured), reused by every log call
# within a request instead of constructing a fresh Logger per call.
class RequestLogging
  def self.configure(tag: String, level: String = "info", output = nil, format: String = "text")
    @@tag = tag
    @@level = level
    @@output = output
    @@format = format
  end

  # The memoized, per-context Logger -- every method below goes through
  # this rather than constructing its own, and app code wanting the
  # same Logger for a one-off log call outside a request (e.g. at
  # startup) can call it directly too.
  def self.get(context)
    logger = context["request_logging_logger"]
    if logger == nil
      logger = Logger.new(@@tag, @@level, @@output, @@format)
      context["request_logging_logger"] = logger
    end
    logger
  end

  # request_id/method/path, plus whatever the caller's own `fields`
  # adds -- the shape every log call within a request wants attached.
  def self.fields(request, fields: Hash = {})
    fields.merge({"request_id": request["request_id"], "method": request["method"], "path": request["path"]})
  end

  def self.debug(request, context, event, fields: Hash = {}) = RequestLogging.get(context).debug(event, RequestLogging.fields(request, fields))
  def self.info(request, context, event, fields: Hash = {}) = RequestLogging.get(context).info(event, RequestLogging.fields(request, fields))
  def self.warn(request, context, event, fields: Hash = {}) = RequestLogging.get(context).warn(event, RequestLogging.fields(request, fields))

  # The correlation Hash the current request's own `.call` (below)
  # stashed on context["log_context"] -- {} if `.call` was never in the
  # chain, or hasn't run yet. Handy for code that wants to tag its own
  # log lines with the same request_id/method/path without needing the
  # `request` Hash itself in scope -- e.g. examples/project_board's
  # build_query_logger, which correlates ActiveRecord query logs with
  # whichever request triggered them this way.
  def self.correlation(context)
    fields = context["log_context"]
    if fields == nil then {} else fields end
  end

  # The Rack-style middleware itself: mints request["request_id"],
  # stashes the correlation Hash .correlation(context) reads back, logs
  # request.started/request.completed around `forward`, and logs
  # request.failed (error_class/error_message/duration_ms) before
  # re-raising if `forward` raises, rather than letting the exception
  # cross this middleware with no record of it. Put this outermost in
  # your rack_compose chain so its timing covers every other
  # middleware's own work too, not just the route handler's.
  def self.call(request, context, forward)
    request["request_id"] = SecureRandom.hex(8)
    context["log_context"] = RequestLogging.fields(request)
    started_at = Time.monotonic()
    RequestLogging.info(request, context, "request.started")
    begin
      response = forward(request, context)
      duration_ms = (Time.monotonic() - started_at) * 1000
      # `nil` is gremlin_serve's own "a handler already fully handled
      # this response itself" signal (see packages/gremlin's own
      # "Escape hatch: taking over the raw connection" doc comment --
      # a WebSocket upgrade is the motivating case), not a bare request
      # this middleware forgot to answer -- response[0] below would
      # otherwise crash on it, so it gets its own distinct event instead
      # of a fabricated status.
      if response == nil
        RequestLogging.info(request, context, "request.upgraded", {"duration_ms": duration_ms})
      else
        RequestLogging.info(request, context, "request.completed", {"status": response[0], "duration_ms": duration_ms})
      end
      context["log_context"] = nil
      response
    rescue error: StandardError
      duration_ms = (Time.monotonic() - started_at) * 1000
      RequestLogging.get(context).error("request.failed", RequestLogging.fields(request, {"error_class": error.class(), "error_message": error.message(), "duration_ms": duration_ms}))
      context["log_context"] = nil
      raise error
    end
  end
end
