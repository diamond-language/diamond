require "../../packages/rack/rack"

def outer_middleware(request, context, forward)
  context["log"].push("outer:before")
  response = forward(request, context)
  context["log"].push("outer:after")
  response
end

def inner_middleware(request, context, forward)
  context["log"].push("inner:before")
  response = forward(request, context)
  context["log"].push("inner:after")
  response
end

def app_handler(request, context)
  context["log"].push("app")
  [200, {}, "ok"]
end

chain = rack_compose([outer_middleware, inner_middleware], app_handler)
context = {"log": []}
response = rack_run_chain(chain, 0, {}, context)
[response, context["log"]]
