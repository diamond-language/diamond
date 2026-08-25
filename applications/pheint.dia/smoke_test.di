require "./boot"

if PheintEnvironment.name() != "test"
  raise "smoke_test.di requires DIAMOND_ENV=test"
end

def smoke_request(method, path, body = "")
  {"method": method, "path": path, "body": body, "headers": {}}
end

def authenticated_graphql(token, query)
  {"method": "POST", "path": "/graphql",
   "body": JSON.stringify({"query": query}),
   "headers": {"authorization": "Bearer #{token}"}}
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


signup_query = [
  "mutation {",
  "  signUp(email: \" Alice@Example.COM \", password: \"correct horse\", handle: \"@Alice\") {",
  "    token account { id email player { handle } }",
  "  }",
  "}"
].join("\n")
signup = app(smoke_request("POST", "/graphql",
  JSON.stringify({"query": signup_query})), context)
signup_json = JSON.parse(signup[2])
if signup[0] != 200 || signup_json["errors"] != nil
  raise "signup failed"
end
signup_data = signup_json["data"]["signUp"]
token = signup_data["token"]
if token == nil || token.length() != 64 ||
   signup_data["account"]["email"] != "alice@example.com" ||
   signup_data["account"]["player"]["handle"] != "alice"
  raise "signup did not normalize identity or return auth payload"
end

anonymous_me = JSON.parse(app(smoke_request("POST", "/graphql",
  JSON.stringify({"query": "{ me { email } }"})), context)[2])
if anonymous_me["data"]["me"] != nil then raise "anonymous me was not nil" end

authenticated_me = JSON.parse(app(authenticated_graphql(token,
  "{ me { email player { handle } } }"), context)[2])
if authenticated_me["data"]["me"]["player"]["handle"] != "alice"
  raise "bearer token did not authenticate me"
end

signout = JSON.parse(app(authenticated_graphql(token,
  "mutation { signOut }"), context)[2])
if signout["data"]["signOut"] != true then raise "signout failed" end

signed_out_me = JSON.parse(app(authenticated_graphql(token,
  "{ me { email } }"), context)[2])
if signed_out_me["data"]["me"] != nil then raise "signed-out token remained valid" end

bad_signin = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
  "query": "mutation { signIn(email: \"alice@example.com\", password: \"wrongpass\") { token } }"
})), context)[2])
if bad_signin["errors"] == nil then raise "invalid signin was accepted" end

signin = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
  "query": "mutation { signIn(email: \"ALICE@EXAMPLE.COM\", password: \"correct horse\") { token account { email } } }"
})), context)[2])
if signin["errors"] != nil || signin["data"]["signIn"]["token"] == nil
  raise "valid signin failed"
end


duplicate_signup = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
  "query": "mutation { signUp(email: \"alice@example.com\", password: \"another pass\", handle: \"other\") { token } }"
})), context)[2])
if duplicate_signup["errors"] == nil then raise "duplicate email signup was accepted" end

invalid_player_signup = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
  "query": "mutation { signUp(email: \"other@example.com\", password: \"another pass\", handle: \"!\") { token } }"
})), context)[2])
if invalid_player_signup["errors"] == nil
  raise "invalid player signup was accepted"
end
if Account.all().count(PheintDatabase.get(context)) != 1 ||
   Player.all().count(PheintDatabase.get(context)) != 1
  raise "failed signup was not rolled back atomically"
end

missing = app(smoke_request("GET", "/missing"), context)
if missing[0] != 404
  raise "missing route smoke test failed"
end

puts("pheint.dia smoke ok")
