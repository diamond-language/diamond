def soak_database_state(path)
  db = SQLite3.open(path)
  begin
    counts = {}
    ["players", "games", "leaderboard_scores", "cloud_saves", "challenge_runs", "notifications", "replays"].each() do |table|
      counts[table] = db.query("SELECT count(*) AS n FROM #{table}")[0]["n"]
    end
    if db.query("PRAGMA integrity_check")[0].values()[0] != "ok" then raise "database integrity failed" end
    if db.query("PRAGMA foreign_key_check").length() != 0 then raise "database foreign keys failed" end
    counts
  ensure
    db.close()
  end
end

def verify_soak_post(world)
  check = WorkloadChecks.new()
  game = world["games"][0]
  anon = Client.new(world["port"])
  page = anon.data(GAME_PAGE, {"s": game["slug"]})["game"]
  check.ok("post-soak nested game remains populated", page["slug"] == game["slug"] && page["posts"].length() > 0 && page["leaderboards"].length() > 0)
  saved = 0
  world["players"].each() do |who|
    client = Client.new(world["port"], who["token"])
    if client.data("{me{id}}")["me"]["id"] != who["id"] then raise "post-soak identity mismatch" end
    row = client.data("query($s:String!){save(gameSlug:$s,slot:\"bench\"){version data}}", {"s": game["slug"]})["save"]
    if row != nil
      if row["version"] < 1 || row["data"] != "QUJD" * 1024 then raise "post-soak save bytes/version mismatch" end
      saved += 1
    end
  end
  check.ok("post-soak identities and loaded save bytes survive", saved > 0)
  player = Client.new(world["port"], world["players"][0]["token"])
  write = "mutation($s:String!,$v:Int!){writeSave(gameSlug:$s,slot:\"soak-final\",data:\"c29hay1maW5hbA==\",expectedVersion:$v){version data}}"
  row = player.data(write, {"s": game["slug"], "v": 0})["writeSave"]
  check.ok("post-soak versioned write persists", row["version"] == 1 && row["data"] == "c29hay1maW5hbA==")
  check.ok("post-soak stale writer is refused", player.code(write, {"s": game["slug"], "v": 0}) == "CONFLICT")
  row = player.data("query($s:String!){save(gameSlug:$s,slot:\"soak-final\"){version data}}", {"s": game["slug"]})["save"]
  check.ok("post-soak conflict preserves stored bytes", row["version"] == 1 && row["data"] == "c29hay1maW5hbA==")
  score = (Time.monotonic() * 1000).to_i() + 1000000
  result = player.data(SCENARIOS["submit_score"][2], {"l": game["arcade"], "s": score})["submitScore"]
  check.ok("post-soak score improvement persists", result["improved"] == true && result["entry"]["score"] == score)
  lower = player.data(SCENARIOS["submit_score"][2], {"l": game["arcade"], "s": score - 1})["submitScore"]
  board = player.data(SCENARIOS["leaderboard_global"][2], {"l": game["arcade"]})["leaderboard"]
  check.ok("post-soak lower score preserves the best", lower["improved"] == false && board["me"]["score"] == score)
  puts("post-soak checked #{world["players"].length()} identities and #{saved} loaded save slots")
  check.count()
end
