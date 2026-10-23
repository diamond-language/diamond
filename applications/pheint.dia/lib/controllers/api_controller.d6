class ApiController
  def self.index(request, context, params)
    [200, {"Content-Type": "application/json"}, JSON.stringify({
      "application": "pheint.dia", "kind": "graphql_api",
      "graphql": "/graphql", "health": "/health",
      "environment": PheintEnvironment.name()
    })]
  end

  def self.health(request, context, params)
    [200, {"Content-Type": "application/json"}, JSON.stringify({
      "status": "ok", "application": "pheint.dia",
      "environment": PheintEnvironment.name()
    })]
  end
end
