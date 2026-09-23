require_cut "registry"
require_cut "gremlin"

root = ENV["REGISTRY_ROOT"]
if root == nil then raise "REGISTRY_ROOT is required" end
startup = SQLite3.open(File.join(root, "registry.db"))
Registry::Schema.apply(startup)
startup.close()

def handle_registry(request, context)
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
  end
  api.call(request)
end
port = ENV["REGISTRY_PORT"]
if port == nil then port = "18120" end
gremlin_serve(port.to_i(), handle_registry)
