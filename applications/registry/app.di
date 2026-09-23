require_cut "registry"
require_cut "gremlin"
require "./catalog"

root = ENV["REGISTRY_ROOT"]
if root == nil then raise "REGISTRY_ROOT is required" end
startup = SQLite3.open(File.join(root, "registry.db"))
Registry::Schema.apply(startup)
startup.close()

def dispatch_registry(request, context)
  api = context["registry_api"]
  if api == nil
    root = ENV["REGISTRY_ROOT"]
    verifier = ENV["REGISTRY_FACET"]
    if verifier == nil then raise "REGISTRY_FACET is required" end
    base = ENV["REGISTRY_BASE"]
    if base == nil then base = "" end
    db = SQLite3.open(File.join(root, "registry.db"))
    store = Registry::BlobStore.new(File.join(root, "blobs"))
    publisher = Registry::Publisher.new(db, store, File.join(root, "staging"), verifier)
    api = Registry::API.new(db, store, publisher, base)
    context["registry_api"] = api
    context["registry_db"] = db
    context["registry_base"] = base
  end
  catalog = registry_catalog(request, context["registry_db"], context["registry_base"])
  if catalog != nil then return catalog end
  api.call(request)
end
def handle_registry(request, context)
  id = request["request_id"]
  started = Time.monotonic()
  log = context["registry_log"]
  if log == nil
    log = Logger.new("registry", "info", nil, "json")
    context["registry_log"] = log
  end
  log.info("request.started", {"request_id": id, "method": request["method"], "request_bytes": request["body"].length()})
  begin
    response = dispatch_registry(request, context)
  rescue error: StandardError
    response = [500, {"Content-Type": "application/json"}, JSON.stringify({"protocol": 1, "error": "internal_error", "message": "internal_error"})]
  end
  response[1]["X-Request-ID"] = id
  response[1]["Connection"] = "close"
  if response[0] >= 400 && response[1]["Content-Type"] == "application/json"
    body = JSON.parse(response[2])
    body["request_id"] = id
    response[2] = JSON.stringify(body)
  end
  fields = {"request_id": id, "method": request["method"], "status": response[0], "duration_ms": (Time.monotonic() - started) * 1000, "response_bytes": response[2].length()}
  if response[0] >= 500
    log.error("request.completed", fields)
  elsif response[0] >= 400
    log.warn("request.completed", fields)
  else
    log.info("request.completed", fields)
  end
  response
end

def registry_setting(name, fallback, maximum)
  text = ENV[name]
  if text == nil then return fallback end
  value = text.to_i()
  if "#{value}" != text || value < 1 || value > maximum then raise ArgumentError.new("invalid registry setting: #{name}") end
  value
end
port = registry_setting("REGISTRY_PORT", 18120, 65535)
limits = {"line_bytes": 8192, "header_bytes": 32768, "header_count": 100,
  "body_bytes": registry_setting("REGISTRY_MAX_BODY_BYTES", 26214400, 58720256),
  "connections": registry_setting("REGISTRY_MAX_CONNECTIONS", 8, 1024),
  "timeout_seconds": registry_setting("REGISTRY_TIMEOUT_SECONDS", 30, 3600)}
Logger.new("registry", "info", nil, "json").info("server.starting", {"port": port, "limits": limits})
gremlin_serve(port, handle_registry, 1, nil, nil, limits)
