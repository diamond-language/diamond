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
if Account.all().count(PheintDatabase.get(context)) != 2 ||
   Player.all().count(PheintDatabase.get(context)) != 2
  raise "failed signup was not rolled back atomically"
end

def test_score_submission(context, token)
db = PheintDatabase.get(context)
cipher = Game.where({"title": "Cipher Sprint"}).first(db)
board = cipher.leaderboards(db)[0]

anonymous = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
  "query": "mutation { submitScore(leaderboardId: #{board.id()}, value: 90) { id } }"
})), context)[2])
if anonymous["errors"] == nil ||
   !anonymous["errors"][0]["message"].include?("authentication required")
  raise "anonymous score submission was accepted"
end

def submit_score_request(context, token, leaderboard_id, value)
  authenticated_graphql(token, [
    "mutation { submitScore(leaderboardId: #{leaderboard_id}, value: #{value}) {",
    "  id value player { handle }",
    "} }"
  ].join("\n"))
end

created = JSON.parse(app(submit_score_request(
  context, token, board.id(), 90), context)[2])
if created["errors"] != nil || created["data"]["submitScore"]["value"] != 90 ||
   created["data"]["submitScore"]["player"]["handle"] != "alice"
  raise "first score submission failed"
end
score_id = created["data"]["submitScore"]["id"]

lower = JSON.parse(app(submit_score_request(
  context, token, board.id(), 89), context)[2])
if lower["errors"] == nil ||
   !lower["errors"][0]["message"].include?("current best of 90")
  raise "lower score submission was accepted"
end
alice = Account.where({"email": "alice@example.com"}).first(db)
stored = Score.where({
  "leaderboard_id": board.id(), "player_id": alice.player(db).id()
}).first(db)
if stored.value() != 90 then raise "lower score changed stored best" end

equal = JSON.parse(app(submit_score_request(
  context, token, board.id(), 90), context)[2])
if equal["errors"] != nil || equal["data"]["submitScore"]["id"] != score_id
  raise "equal score submission was not idempotent"
end

higher = JSON.parse(app(submit_score_request(
  context, token, board.id(), 125), context)[2])
if higher["errors"] != nil || higher["data"]["submitScore"]["id"] != score_id ||
   higher["data"]["submitScore"]["value"] != 125 ||
   board.scores(db).length() != 1
  raise "higher score did not update the existing score"
end

missing = JSON.parse(app(submit_score_request(
  context, token, 999999, 125), context)[2])
if missing["errors"] == nil ||
   !missing["errors"][0]["message"].include?("leaderboard not found")
  raise "missing leaderboard accepted a score"
end
end

test_score_submission(context, signin["data"]["signIn"]["token"])

def test_seeded_game_domain(context)
demo = Account.where({"email": "demo@pheint.dia"}).first(PheintDatabase.get(context))
if demo == nil || demo.player(PheintDatabase.get(context)).handle() != "demo"
  raise "seeded account/player missing"
end
games = demo.games(PheintDatabase.get(context))
if games.length() != 2 then raise "seeded games missing" end
asteroid = Game.where({"title": "Asteroid Run"}).first(PheintDatabase.get(context))
boards = asteroid.leaderboards(PheintDatabase.get(context))
if boards.length() != 1 || boards[0].name() != "All-time high score"
  raise "seeded leaderboard missing"
end
scores = boards[0].scores(PheintDatabase.get(context))
if scores.length() != 1 || scores[0].value() != 128400 ||
   scores[0].player(PheintDatabase.get(context)).handle() != "demo"
  raise "seeded score/player association missing"
end

duplicate_score_rejected = false
begin
  Score.new({"leaderboard_id": boards[0].id(),
    "player_id": demo.player(PheintDatabase.get(context)).id(),
    "value": 1}).save(PheintDatabase.get(context))
rescue error: StandardError
  duplicate_score_rejected = true
end
if !duplicate_score_rejected ||
   boards[0].scores(PheintDatabase.get(context)).length() != 1
  raise "duplicate per-player leaderboard score was accepted"
end


games_query = [
  "{ games {",
  "  title description",
  "  owner { email player { handle } }",
  "  leaderboards { name scores { value player { handle } } }",
  "} }"
].join("\n")
games_response = JSON.parse(app(smoke_request("POST", "/graphql",
  JSON.stringify({"query": games_query})), context)[2])
if games_response["errors"] != nil || games_response["data"]["games"].length() != 2
  raise "GraphQL games query failed"
end
graphql_game = games_response["data"]["games"][0]
if graphql_game["owner"]["player"]["handle"] != "demo" ||
   graphql_game["leaderboards"][0]["scores"][0]["value"] != 128400 ||
   graphql_game["leaderboards"][0]["scores"][0]["player"]["handle"] != "demo"
  raise "GraphQL game associations did not resolve"
end
end

test_seeded_game_domain(context)

missing = app(smoke_request("GET", "/missing"), context)
if missing[0] != 404
  raise "missing route smoke test failed"
end

puts("pheint.dia smoke ok")
