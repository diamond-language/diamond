def build_pheint_router()
  router = Dials::Router.new()
  router.get("/", HomeController.index)
  router.get("/health", HomeController.health)
  router
end

def pheint_route(request, context)
  Dials::RouterHolder.get(build_pheint_router).dispatch(request, context)
end
