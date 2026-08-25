require "./boot"

if PheintEnvironment.name() != "test"
  raise "smoke_test.di requires DIAMOND_ENV=test"
end

def smoke_request(method, path, body = "")
  {"method": method, "path": path, "body": body, "headers": {}}
end

context = {"log_level": "off"}
root = app(smoke_request("GET", "/"), context)
if root[0] != 200 || root[1]["Content-Type"] != "application/json" ||
   !root[2].include?("\"kind\":\"graphql_api\"")
  raise "API root smoke test failed"
end

health = app(smoke_request("GET", "/health"), context)
if health[0] != 200 || health[1]["Content-Type"] != "application/json" ||
   !health[2].include?("\"status\":\"ok\"")
  raise "health endpoint smoke test failed"
end

graphql = app(smoke_request("POST", "/graphql", JSON.stringify({
  "query": "{ apiName environment }"
})), context)
if graphql[0] != 200 || graphql[1]["Content-Type"] != "application/json" ||
   !graphql[2].include?("\"apiName\":\"pheint.dia\"") ||
   !graphql[2].include?("\"environment\":\"test\"")
  raise "GraphQL endpoint smoke test failed"
end

invalid_graphql = app(smoke_request("POST", "/graphql", "not json"), context)
if invalid_graphql[0] != 400 || !invalid_graphql[2].include?("valid JSON")
  raise "invalid GraphQL request was not rejected"
end

missing = app(smoke_request("GET", "/missing"), context)
if missing[0] != 404
  raise "missing route smoke test failed"
end

puts("pheint.dia smoke ok")
