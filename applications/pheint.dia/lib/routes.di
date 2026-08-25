def build_pheint_router()
  router = Dials::Router.new()
  router.get("/", ApiController.index)
  router.get("/health", ApiController.health)
  router.post("/graphql", GraphqlController.execute)
  router
end

def pheint_route(request, context)
  Dials::RouterHolder.get(build_pheint_router).dispatch(request, context)
end
