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

worse = JSON.parse(app(submit_score_request(
  context, token, board.id(), 91), context)[2])
if worse["errors"] == nil ||
   !worse["errors"][0]["message"].include?("current best of 90")
  raise "slower time submission was accepted"
end
alice = Account.where({"email": "alice@example.com"}).first(db)
stored = Score.where({
  "leaderboard_id": board.id(), "player_id": alice.player(db).id()
}).first(db)
if stored.value() != 90 then raise "slower time changed stored best" end

equal = JSON.parse(app(submit_score_request(
  context, token, board.id(), 90), context)[2])
if equal["errors"] != nil || equal["data"]["submitScore"]["id"] != score_id
  raise "equal score submission was not idempotent"
end

better = JSON.parse(app(submit_score_request(
  context, token, board.id(), 80), context)[2])
if better["errors"] != nil || better["data"]["submitScore"]["id"] != score_id ||
   better["data"]["submitScore"]["value"] != 80 ||
   board.scores(db).length() != 1
  raise "faster time did not update the existing score"
end

asteroid = Game.where({"title": "Asteroid Run"}).first(db).leaderboards(db)[0]
high_created = JSON.parse(app(submit_score_request(
  context, token, asteroid.id(), 140000), context)[2])
high_worse = JSON.parse(app(submit_score_request(
  context, token, asteroid.id(), 130000), context)[2])
high_better = JSON.parse(app(submit_score_request(
  context, token, asteroid.id(), 150000), context)[2])
if high_created["errors"] != nil || high_worse["errors"] == nil ||
   high_better["errors"] != nil ||
   high_better["data"]["submitScore"]["value"] != 150000
  raise "higher-is-better score policy failed"
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
if boards.length() != 1 || boards[0].name() != "All-time high score" ||
   boards[0].higher_is_better() != true
  raise "seeded leaderboard missing"
end
cipher = Game.where({"title": "Cipher Sprint"}).first(PheintDatabase.get(context))
if cipher.leaderboards(PheintDatabase.get(context))[0].higher_is_better() != false
  raise "lower-is-better leaderboard policy missing"
end
scores = boards[0].scores(PheintDatabase.get(context))
if scores.length() != 2 || scores[0].value() != 128400 ||
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
   boards[0].scores(PheintDatabase.get(context)).length() != 2
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
   graphql_game["leaderboards"][0]["scores"][1]["value"] != 128400 ||
   graphql_game["leaderboards"][0]["scores"][1]["player"]["handle"] != "demo"
  raise "GraphQL game associations did not resolve"
end
end

test_seeded_game_domain(context)

def test_public_player_and_leaderboard_queries(context)
  db = PheintDatabase.get(context)
  asteroid = Game.where({"title": "Asteroid Run"}).first(db)
  board = asteroid.leaderboards(db)[0]
  leaderboard_response = JSON.parse(app(smoke_request(
    "POST", "/graphql", JSON.stringify({"query": [
      "{ leaderboard(id: #{board.id()}) {",
      "  name higherIsBetter game { title } scores { value player { handle } }",
      "} }"
    ].join("\n")})), context)[2])
  leaderboard = leaderboard_response["data"]["leaderboard"]
  if leaderboard_response["errors"] != nil ||
     leaderboard["higherIsBetter"] != true ||
     leaderboard["game"]["title"] != "Asteroid Run" ||
     leaderboard["scores"].length() != 2 ||
     leaderboard["scores"][0]["value"] != 150000 ||
     leaderboard["scores"][0]["player"]["handle"] != "alice" ||
     leaderboard["scores"][1]["value"] != 128400
    raise "leaderboard query did not return descending rankings"
  end

  player_response = JSON.parse(app(smoke_request(
    "POST", "/graphql", JSON.stringify({"query": [
      "{ player(handle: \"@demo\") {",
      "  handle scores { value leaderboard { name game { title } } }",
      "} }"
    ].join("\n")})), context)[2])
  player = player_response["data"]["player"]
  if player_response["errors"] != nil || player["handle"] != "demo" ||
     player["scores"].length() != 1 || player["scores"][0]["value"] != 128400 ||
     player["scores"][0]["leaderboard"]["game"]["title"] != "Asteroid Run"
    raise "player profile query did not resolve score history"
  end

  missing_player = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "{ player(handle: \"missing\") { id } }"
  })), context)[2])
  missing_board = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "{ leaderboard(id: 999999) { id } }"
  })), context)[2])
  if missing_player["errors"] != nil || missing_player["data"]["player"] != nil ||
     missing_board["errors"] != nil || missing_board["data"]["leaderboard"] != nil
    raise "missing public profile lookup did not resolve to null"
  end
end

test_public_player_and_leaderboard_queries(context)

def test_query_pagination(context)
  games = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "{ games(limit: 1, offset: 1) { title } }"
  })), context)[2])
  if games["errors"] != nil || games["data"]["games"].length() != 1 ||
     games["data"]["games"][0]["title"] != "Cipher Sprint"
    raise "game pagination was not stable"
  end

  db = PheintDatabase.get(context)
  board = Game.where({"title": "Asteroid Run"}).first(db).leaderboards(db)[0]
  rankings = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "{ leaderboard(id: #{board.id()}) { scores(limit: 1, offset: 1) { value } } }"
  })), context)[2])
  if rankings["errors"] != nil ||
     rankings["data"]["leaderboard"]["scores"].length() != 1 ||
     rankings["data"]["leaderboard"]["scores"][0]["value"] != 128400
    raise "ranking pagination did not run after descending ordering"
  end

  invalid_limit = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "{ games(limit: 101) { id } }"
  })), context)[2])
  invalid_offset = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "{ games(offset: -1) { id } }"
  })), context)[2])
  if invalid_limit["errors"] == nil ||
     !invalid_limit["errors"][0]["message"].include?("limit must be") ||
     invalid_offset["errors"] == nil ||
     !invalid_offset["errors"][0]["message"].include?("offset must be")
    raise "invalid pagination bounds were accepted"
  end
end

test_query_pagination(context)

def test_game_query(context)
  db = PheintDatabase.get(context)
  asteroid = Game.where({"title": "Asteroid Run"}).first(db)
  response = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": [
      "query { game(id: #{asteroid.id()}) {",
      "  id title owner { player { handle } }",
      "  leaderboards { name scores { value player { handle } } }",
      "} }"
    ].join("\n")
  })), context)[2])
  game = response["data"]["game"]
  if response["errors"] != nil || game["title"] != "Asteroid Run" ||
     game["owner"]["player"]["handle"] != "demo" ||
     game["leaderboards"][0]["scores"].length() != 2 ||
     game["leaderboards"][0]["scores"][1]["value"] != 128400 ||
     game["leaderboards"][0]["scores"][1]["player"]["handle"] != "demo"
    raise "single game query did not resolve its requested graph"
  end

  missing = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "{ game(id: 999999) { id } }"
  })), context)[2])
  if missing["errors"] != nil || missing["data"]["game"] != nil
    raise "missing game did not resolve to null"
  end
end

test_game_query(context)

def create_game_request(token, title, description, leaderboard_name)
  authenticated_graphql(token, [
    "mutation { createGame(",
    "  title: \"#{title}\", description: \"#{description}\",",
    "  leaderboardName: \"#{leaderboard_name}\"",
    ") { id title description owner { email } leaderboards { id name } } }"
  ].join("\n"))
end

def test_game_creation(context, token)
  db = PheintDatabase.get(context)
  before_games = Game.all().count(db)
  before_boards = Leaderboard.all().count(db)

  anonymous = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "mutation { createGame(title: \"Nope\", description: \"Nope\", leaderboardName: \"Nope\") { id } }"
  })), context)[2])
  if anonymous["errors"] == nil ||
     !anonymous["errors"][0]["message"].include?("authentication required")
    raise "anonymous game creation was accepted"
  end

  invalid = JSON.parse(app(create_game_request(
    token, "Rollback Me", "A valid description", ""), context)[2])
  if invalid["errors"] == nil || Game.all().count(db) != before_games ||
     Leaderboard.all().count(db) != before_boards
    raise "invalid initial leaderboard did not roll back game creation"
  end

  created = JSON.parse(app(create_game_request(
    token, "Orbit Forge", "Build stations in a shifting orbit.", "Most stations"), context)[2])
  game = created["data"]["createGame"]
  if created["errors"] != nil || game["title"] != "Orbit Forge" ||
     game["owner"]["email"] != "alice@example.com" ||
     game["leaderboards"].length() != 1 ||
     game["leaderboards"][0]["name"] != "Most stations" ||
     Game.all().count(db) != before_games + 1 ||
     Leaderboard.all().count(db) != before_boards + 1
    raise "authenticated game creation failed"
  end
end

test_game_creation(context, signin["data"]["signIn"]["token"])

def create_leaderboard_request(token, game_id, name)
  authenticated_graphql(token,
    "mutation { createLeaderboard(gameId: #{game_id}, name: \"#{name}\") { id name } }")
end

def test_leaderboard_creation(context, token)
  db = PheintDatabase.get(context)
  owned = Game.where({"title": "Orbit Forge"}).first(db)
  foreign = Game.where({"title": "Asteroid Run"}).first(db)
  before_boards = Leaderboard.all().count(db)

  forbidden = JSON.parse(app(create_leaderboard_request(
    token, foreign.id(), "Cheaters"), context)[2])
  if forbidden["errors"] == nil ||
     !forbidden["errors"][0]["message"].include?("only the game owner") ||
     Leaderboard.all().count(db) != before_boards
    raise "non-owner created a leaderboard"
  end

  missing = JSON.parse(app(create_leaderboard_request(
    token, 999999, "Missing"), context)[2])
  if missing["errors"] == nil ||
     !missing["errors"][0]["message"].include?("game not found")
    raise "missing game accepted a leaderboard"
  end

  invalid = JSON.parse(app(create_leaderboard_request(
    token, owned.id(), ""), context)[2])
  if invalid["errors"] == nil || Leaderboard.all().count(db) != before_boards
    raise "invalid leaderboard was persisted"
  end

  created = JSON.parse(app(create_leaderboard_request(
    token, owned.id(), "Fastest completion"), context)[2])
  if created["errors"] != nil ||
     created["data"]["createLeaderboard"]["name"] != "Fastest completion" ||
     Leaderboard.all().count(db) != before_boards + 1
    raise "game owner could not create a leaderboard"
  end
end

test_leaderboard_creation(context, signin["data"]["signIn"]["token"])

def test_my_games_query(context, token)
  anonymous = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "{ myGames { id } }"
  })), context)[2])
  if anonymous["errors"] == nil ||
     !anonymous["errors"][0]["message"].include?("authentication required")
    raise "anonymous myGames query was accepted"
  end

  response = JSON.parse(app(authenticated_graphql(token,
    "{ myGames { title leaderboards { name } } }"), context)[2])
  games = response["data"]["myGames"]
  if response["errors"] != nil || games.length() != 1 ||
     games[0]["title"] != "Orbit Forge" || games[0]["leaderboards"].length() != 2
    raise "myGames did not return only the authenticated owner's games"
  end
end

test_my_games_query(context, signin["data"]["signIn"]["token"])

def update_game_request(token, game_id, title, description)
  authenticated_graphql(token, [
    "mutation { updateGame(id: #{game_id}, title: \"#{title}\",",
    "  description: \"#{description}\") { id title description } }"
  ].join("\n"))
end

def test_game_update(context, token)
  db = PheintDatabase.get(context)
  owned = Game.where({"title": "Orbit Forge"}).first(db)
  foreign = Game.where({"title": "Asteroid Run"}).first(db)

  forbidden = JSON.parse(app(update_game_request(
    token, foreign.id(), "Stolen", "Nope"), context)[2])
  if forbidden["errors"] == nil ||
     !forbidden["errors"][0]["message"].include?("only the game owner") ||
     Game.find(db, foreign.id()).title() != "Asteroid Run"
    raise "non-owner updated a game"
  end

  invalid = JSON.parse(app(update_game_request(
    token, owned.id(), "", "Still valid"), context)[2])
  if invalid["errors"] == nil || Game.find(db, owned.id()).title() != "Orbit Forge"
    raise "invalid game update was persisted"
  end

  updated = JSON.parse(app(update_game_request(
    token, owned.id(), "Orbit Foundry", "Build and defend orbital stations."), context)[2])
  game = updated["data"]["updateGame"]
  if updated["errors"] != nil || game["title"] != "Orbit Foundry" ||
     game["description"] != "Build and defend orbital stations." ||
     Game.find(db, owned.id()).title() != "Orbit Foundry"
    raise "game owner could not update a game"
  end
end

test_game_update(context, signin["data"]["signIn"]["token"])

def update_leaderboard_request(token, leaderboard_id, name, higher_is_better)
  authenticated_graphql(token,
    "mutation { updateLeaderboard(id: #{leaderboard_id}, name: \"#{name}\", higherIsBetter: #{higher_is_better}) { id name higherIsBetter } }")
end

def delete_leaderboard_request(token, leaderboard_id)
  authenticated_graphql(token,
    "mutation { deleteLeaderboard(id: #{leaderboard_id}) }")
end

def test_leaderboard_management(context, token)
  db = PheintDatabase.get(context)
  owned_game = Game.where({"title": "Orbit Foundry"}).first(db)
  owned_board = Leaderboard.where({"name": "Fastest completion"}).first(db)
  foreign_board = Leaderboard.where({"name": "All-time high score"}).first(db)

  forbidden_update = JSON.parse(app(update_leaderboard_request(
    token, foreign_board.id(), "Stolen", true), context)[2])
  if forbidden_update["errors"] == nil ||
     !forbidden_update["errors"][0]["message"].include?("only the game owner") ||
     Leaderboard.find(db, foreign_board.id()).name() != "All-time high score"
    raise "non-owner updated a leaderboard"
  end

  invalid = JSON.parse(app(update_leaderboard_request(
    token, owned_board.id(), "", true), context)[2])
  if invalid["errors"] == nil ||
     Leaderboard.find(db, owned_board.id()).name() != "Fastest completion"
    raise "invalid leaderboard update was persisted"
  end

  updated = JSON.parse(app(update_leaderboard_request(
    token, owned_board.id(), "Speed run", false), context)[2])
  if updated["errors"] != nil ||
     updated["data"]["updateLeaderboard"]["name"] != "Speed run" ||
     updated["data"]["updateLeaderboard"]["higherIsBetter"] != false
    raise "game owner could not update a leaderboard"
  end

  forbidden_delete = JSON.parse(app(delete_leaderboard_request(
    token, foreign_board.id()), context)[2])
  if forbidden_delete["errors"] == nil || Leaderboard.find(db, foreign_board.id()) == nil
    raise "non-owner deleted a leaderboard"
  end

  Score.new({
    "leaderboard_id": owned_board.id(),
    "player_id": Account.where({"email": "alice@example.com"}).first(db).player(db).id(),
    "value": 7
  }).save(db)
  deleted = JSON.parse(app(delete_leaderboard_request(token, owned_board.id()), context)[2])
  if deleted["errors"] != nil || deleted["data"]["deleteLeaderboard"] != true ||
     Leaderboard.where({"id": owned_board.id()}).first(db) != nil ||
     Score.where({"leaderboard_id": owned_board.id()}).count(db) != 0 ||
     owned_game.leaderboards(db).length() != 1
    raise "leaderboard deletion did not cascade through scores"
  end
end

test_leaderboard_management(context, signin["data"]["signIn"]["token"])

def delete_game_request(token, game_id)
  authenticated_graphql(token, "mutation { deleteGame(id: #{game_id}) }")
end

def test_game_deletion(context, token)
  db = PheintDatabase.get(context)
  owned = Game.where({"title": "Orbit Foundry"}).first(db)
  foreign = Game.where({"title": "Asteroid Run"}).first(db)
  owned_board_count = owned.leaderboards(db).length()
  before_games = Game.all().count(db)
  before_boards = Leaderboard.all().count(db)

  forbidden = JSON.parse(app(delete_game_request(token, foreign.id()), context)[2])
  if forbidden["errors"] == nil ||
     !forbidden["errors"][0]["message"].include?("only the game owner") ||
     Game.find(db, foreign.id()) == nil
    raise "non-owner deleted a game"
  end

  deleted = JSON.parse(app(delete_game_request(token, owned.id()), context)[2])
  if deleted["errors"] != nil || deleted["data"]["deleteGame"] != true ||
     Game.where({"id": owned.id()}).first(db) != nil ||
     Game.all().count(db) != before_games - 1 ||
     Leaderboard.all().count(db) != before_boards - owned_board_count
    raise "game deletion did not cascade through leaderboards"
  end
end

test_game_deletion(context, signin["data"]["signIn"]["token"])

def update_handle_request(token, handle)
  authenticated_graphql(token,
    "mutation { updateHandle(handle: \"#{handle}\") { id handle } }")
end

def change_password_request(token, current_password, new_password)
  authenticated_graphql(token, [
    "mutation { changePassword(",
    "  currentPassword: \"#{current_password}\",",
    "  newPassword: \"#{new_password}\"",
    ") }"
  ].join("\n"))
end

def test_account_settings(context, token)
  db = PheintDatabase.get(context)
  alice = Account.where({"email": "alice@example.com"}).first(db)
  extra_signin = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "mutation { signIn(email: \"alice@example.com\", password: \"correct horse\") { token } }"
  })), context)[2])
  extra_token = extra_signin["data"]["signIn"]["token"]

  duplicate = JSON.parse(app(update_handle_request(token, "@demo"), context)[2])
  if duplicate["errors"] == nil || alice.player(db).handle() != "alice"
    raise "duplicate handle update was accepted"
  end

  invalid = JSON.parse(app(update_handle_request(token, "!"), context)[2])
  if invalid["errors"] == nil || alice.player(db).handle() != "alice"
    raise "invalid handle update was accepted"
  end

  updated = JSON.parse(app(update_handle_request(token, "@Alice_One"), context)[2])
  if updated["errors"] != nil ||
     updated["data"]["updateHandle"]["handle"] != "alice_one" ||
     alice.player(db).handle() != "alice_one"
    raise "handle update did not normalize and persist"
  end

  unchanged = JSON.parse(app(update_handle_request(token, "alice_one"), context)[2])
  if unchanged["errors"] != nil ||
     unchanged["data"]["updateHandle"]["handle"] != "alice_one"
    raise "idempotent handle update failed"
  end

  wrong = JSON.parse(app(change_password_request(
    token, "wrong password", "a replacement password"), context)[2])
  short = JSON.parse(app(change_password_request(
    token, "correct horse", "short"), context)[2])
  if wrong["errors"] == nil || short["errors"] == nil ||
     !alice.authenticate("correct horse")
    raise "invalid password change altered the password"
  end

  changed = JSON.parse(app(change_password_request(
    token, "correct horse", "a replacement password"), context)[2])
  changed_account = Account.find(db, alice.id())
  if changed["errors"] != nil || changed["data"]["changePassword"] != true ||
     changed_account.authenticate("correct horse") ||
     !changed_account.authenticate("a replacement password")
    raise "password change did not replace the digest"
  end
  revoked = JSON.parse(app(authenticated_graphql(extra_token,
    "{ me { id } }"), context)[2])
  retained = JSON.parse(app(authenticated_graphql(token,
    "{ me { id } }"), context)[2])
  if revoked["data"]["me"] != nil || retained["data"]["me"] == nil
    raise "password change did not revoke other sessions and retain the current one"
  end

  old_signin = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "mutation { signIn(email: \"alice@example.com\", password: \"correct horse\") { token } }"
  })), context)[2])
  new_signin = JSON.parse(app(smoke_request("POST", "/graphql", JSON.stringify({
    "query": "mutation { signIn(email: \"alice@example.com\", password: \"a replacement password\") { token } }"
  })), context)[2])
  if old_signin["errors"] == nil || new_signin["errors"] != nil ||
     new_signin["data"]["signIn"]["token"] == nil
    raise "signin did not adopt the changed password"
  end
end

test_account_settings(context, signin["data"]["signIn"]["token"])

missing = app(smoke_request("GET", "/missing"), context)
if missing[0] != 404
  raise "missing route smoke test failed"
end

puts("pheint.dia smoke ok")
