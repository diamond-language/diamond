require "../../packages/rack/lib/rack"

def auth_middleware(request, context, forward)
  if request["headers"]["authorization"] == "Bearer secret"
    forward(request, context)
  else
    [401, {"Content-Type": "text/plain"}, "unauthorized"]
  end
end

def app_handler(request, context)
  context["app_ran"] = true
  [200, {}, "ok"]
end

chain = rack_compose([auth_middleware], app_handler)

authorized = {"headers": {"authorization": "Bearer secret"}}
unauthorized = {"headers": {"authorization": "nope"}}

ok_context = {}
ok_response = rack_run_chain(chain, 0, authorized, ok_context)

blocked_context = {}
blocked_response = rack_run_chain(chain, 0, unauthorized, blocked_context)

[ok_response, ok_context["app_ran"], blocked_response, blocked_context["app_ran"]]
