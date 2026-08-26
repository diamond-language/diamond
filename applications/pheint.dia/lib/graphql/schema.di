def pheint_page_limit(args)
  limit = args["limit"]
  if limit < 1 || limit > 100
    raise GraphQL::ExecutionError.new("limit must be between 1 and 100")
  end
  limit
end

def pheint_page_offset(args)
  offset = args["offset"]
  if offset < 0
    raise GraphQL::ExecutionError.new("offset must be non-negative")
  end
  offset
end

module PheintPlayerResolvers
  module_function
  def id(player, args, context) = player.id()
  def handle(player, args, context) = player.handle()
  def scores(player, args, context)
    values = if player.association_loaded?("scores")
      player.preloaded_association("scores")
    else
      player.scores(context["db"])
    end
    ordered = values.sort_by() do |score|
      -score.value()
    end
    ordered.drop(pheint_page_offset(args)).take(pheint_page_limit(args))
  end
end

module PheintScoreResolvers
  module_function
  def id(score, args, context) = score.id()
  def value(score, args, context) = score.value()
  def leaderboard(score, args, context)
    if score.association_loaded?("leaderboard")
      score.preloaded_association("leaderboard")
    else
      score.leaderboard(context["db"])
    end
  end
  def player(score, args, context)
    if score.association_loaded?("player")
      score.preloaded_association("player")
    else
      score.player(context["db"])
    end
  end
end

module PheintLeaderboardResolvers
  module_function
  def id(leaderboard, args, context) = leaderboard.id()
  def name(leaderboard, args, context) = leaderboard.name()
  def game(leaderboard, args, context)
    if leaderboard.association_loaded?("game")
      leaderboard.preloaded_association("game")
    else
      leaderboard.game(context["db"])
    end
  end
  def scores(leaderboard, args, context)
    values = if leaderboard.association_loaded?("scores")
      leaderboard.preloaded_association("scores")
    else
      leaderboard.scores(context["db"])
    end
    ordered = values.sort_by() do |score|
      -score.value()
    end
    ordered.drop(pheint_page_offset(args)).take(pheint_page_limit(args))
  end
end

module PheintGameResolvers
  module_function
  def id(game, args, context) = game.id()
  def title(game, args, context) = game.title()
  def description(game, args, context) = game.description()
  def owner(game, args, context)
    if game.association_loaded?("owner")
      game.preloaded_association("owner")
    else
      game.owner(context["db"])
    end
  end
  def leaderboards(game, args, context)
    if game.association_loaded?("leaderboards")
      game.preloaded_association("leaderboards")
    else
      game.leaderboards(context["db"])
    end
  end
end

module PheintAccountResolvers
  module_function
  def id(account, args, context) = account.id()
  def email(account, args, context) = account.email()
  def player(account, args, context) = account.player(context["db"])
end

module PheintAuthPayloadResolvers
  module_function
  def token(payload, args, context) = payload["token"]
  def account(payload, args, context) = payload["account"]
end

module PheintQueryResolvers
  module_function
  def api_name(object, args, context) = "pheint.dia"
  def environment(object, args, context) = PheintEnvironment.name()
  def me(object, args, context) = context["current_account"]
  def player(object, args, context)
    handle = pheint_normalize_handle(args["handle"])
    Player.where({"handle": handle}).first(context["db"])
  end
  def leaderboard(object, args, context)
    leaderboard_id = args["id"]
    if leaderboard_id is String then leaderboard_id = leaderboard_id.to_i() end
    Leaderboard.where({"id": leaderboard_id}).first(context["db"])
  end
  def game(object, args, context)
    game_id = args["id"]
    if game_id is String then game_id = game_id.to_i() end
    planned = GraphSQL.resolve(Game.where({"id": game_id}), context["db"],
      context["lookahead"], PheintGraphSQLMappings.games())
    if planned is ActiveRecord::Relation
      planned.first(context["db"])
    elsif planned.length() == 0
      nil
    else
      planned[0]
    end
  end
  def my_games(object, args, context)
    account = context["current_account"]
    if account == nil
      raise GraphQL::ExecutionError.new("authentication required")
    end
    relation = Game.where({"owner_id": account.id()})
    relation = relation.order(Arel.table("games").column("id").asc())
    relation = relation.skip(pheint_page_offset(args)).limit(pheint_page_limit(args))
    planned = GraphSQL.resolve(relation,
      context["db"], context["lookahead"], PheintGraphSQLMappings.games())
    if planned is ActiveRecord::Relation then planned.to_a(context["db"]) else planned end
  end
  def games(object, args, context)
    relation = Game.all().order(Arel.table("games").column("id").asc())
    relation = relation.skip(pheint_page_offset(args)).limit(pheint_page_limit(args))
    planned = GraphSQL.resolve(relation, context["db"], context["lookahead"],
      PheintGraphSQLMappings.games())
    if planned is ActiveRecord::Relation then planned.to_a(context["db"]) else planned end
  end
end

module PheintMutationResolvers
  module_function

  def change_password(object, args, context)
    account = context["current_account"]
    if account == nil
      raise GraphQL::ExecutionError.new("authentication required")
    end
    unless account.authenticate(args["currentPassword"])
      raise GraphQL::ExecutionError.new("current password is incorrect")
    end
    new_password = args["newPassword"]
    if new_password.length() < 8 || new_password.length() > 72
      raise GraphQL::ExecutionError.new("password must be between 8 and 72 characters")
    end
    cost = if PheintEnvironment.name() == "test" then 4 else 12 end
    digest = BCrypt.hash(new_password, cost)
    context["db"].execute("UPDATE accounts SET password_digest = ? WHERE id = ?",
      [digest, account.id()])
    account.password_digest = digest
    current_session = context["current_session"]
    context["db"].execute(
      "DELETE FROM sessions WHERE account_id = ? AND id != ?",
      [account.id(), current_session.id()])
    pheint_audit_info(context, "authentication.password_changed", {
      "account_id": account.id(), "session_id": current_session.id()
    })
    true
  end

  def update_handle(object, args, context)
    account = context["current_account"]
    if account == nil
      raise GraphQL::ExecutionError.new("authentication required")
    end
    db = context["db"]
    player = account.player(db)
    handle = pheint_normalize_handle(args["handle"])
    if handle == player.handle() then return player end
    player.handle = handle
    begin
      player.save(db)
    rescue error: ActiveRecord::ValidationError
      raise GraphQL::ExecutionError.new(error.errors().join(", "))
    end
    pheint_audit_info(context, "player.handle_updated", {
      "account_id": account.id(), "player_id": player.id()
    })
    player
  end

  def delete_leaderboard(object, args, context)
    account = context["current_account"]
    if account == nil
      raise GraphQL::ExecutionError.new("authentication required")
    end
    db = context["db"]
    leaderboard_id = args["id"]
    if leaderboard_id is String then leaderboard_id = leaderboard_id.to_i() end
    leaderboard = Leaderboard.where({"id": leaderboard_id}).first(db)
    if leaderboard == nil
      raise GraphQL::ExecutionError.new("leaderboard not found")
    end
    game = leaderboard.game(db)
    if game.owner_id() != account.id()
      raise GraphQL::ExecutionError.new("only the game owner can delete its leaderboards")
    end
    deleted_leaderboard_id = leaderboard.id()
    leaderboard.destroy(db)
    pheint_audit_info(context, "leaderboard.deleted", {
      "account_id": account.id(), "game_id": game.id(),
      "leaderboard_id": deleted_leaderboard_id
    })
    true
  end

  def update_leaderboard(object, args, context)
    account = context["current_account"]
    if account == nil
      raise GraphQL::ExecutionError.new("authentication required")
    end
    db = context["db"]
    leaderboard_id = args["id"]
    if leaderboard_id is String then leaderboard_id = leaderboard_id.to_i() end
    leaderboard = Leaderboard.where({"id": leaderboard_id}).first(db)
    if leaderboard == nil
      raise GraphQL::ExecutionError.new("leaderboard not found")
    end
    game = leaderboard.game(db)
    if game.owner_id() != account.id()
      raise GraphQL::ExecutionError.new("only the game owner can update its leaderboards")
    end
    leaderboard.name = args["name"]
    begin
      leaderboard.save(db)
    rescue error: ActiveRecord::ValidationError
      raise GraphQL::ExecutionError.new(error.errors().join(", "))
    end
    pheint_audit_info(context, "leaderboard.updated", {
      "account_id": account.id(), "game_id": game.id(),
      "leaderboard_id": leaderboard.id()
    })
    leaderboard
  end

  def delete_game(object, args, context)
    account = context["current_account"]
    if account == nil
      raise GraphQL::ExecutionError.new("authentication required")
    end
    db = context["db"]
    game_id = args["id"]
    if game_id is String then game_id = game_id.to_i() end
    game = Game.where({"id": game_id}).first(db)
    if game == nil
      raise GraphQL::ExecutionError.new("game not found")
    end
    if game.owner_id() != account.id()
      raise GraphQL::ExecutionError.new("only the game owner can delete it")
    end
    deleted_game_id = game.id()
    game.destroy(db)
    pheint_audit_info(context, "game.deleted", {
      "account_id": account.id(), "game_id": deleted_game_id
    })
    true
  end

  def update_game(object, args, context)
    account = context["current_account"]
    if account == nil
      raise GraphQL::ExecutionError.new("authentication required")
    end
    db = context["db"]
    game_id = args["id"]
    if game_id is String then game_id = game_id.to_i() end
    game = Game.where({"id": game_id}).first(db)
    if game == nil
      raise GraphQL::ExecutionError.new("game not found")
    end
    if game.owner_id() != account.id()
      raise GraphQL::ExecutionError.new("only the game owner can update it")
    end
    game.title = args["title"]
    game.description = args["description"]
    begin
      game.save(db)
    rescue error: ActiveRecord::ValidationError
      raise GraphQL::ExecutionError.new(error.errors().join(", "))
    end
    pheint_audit_info(context, "game.updated", {
      "account_id": account.id(), "game_id": game.id()
    })
    game
  end

  def create_leaderboard(object, args, context)
    account = context["current_account"]
    if account == nil
      raise GraphQL::ExecutionError.new("authentication required")
    end
    db = context["db"]
    game_id = args["gameId"]
    if game_id is String then game_id = game_id.to_i() end
    game = Game.where({"id": game_id}).first(db)
    if game == nil
      raise GraphQL::ExecutionError.new("game not found")
    end
    if game.owner_id() != account.id()
      raise GraphQL::ExecutionError.new("only the game owner can create leaderboards")
    end
    leaderboard = Leaderboard.new({"game_id": game.id(), "name": args["name"]})
    begin
      leaderboard.save(db)
    rescue error: ActiveRecord::ValidationError
      raise GraphQL::ExecutionError.new(error.errors().join(", "))
    end
    pheint_audit_info(context, "leaderboard.created", {
      "account_id": account.id(), "game_id": game.id(),
      "leaderboard_id": leaderboard.id()
    })
    leaderboard
  end

  def create_game(object, args, context)
    account = context["current_account"]
    if account == nil
      raise GraphQL::ExecutionError.new("authentication required")
    end

    db = context["db"]
    game = Game.new({
      "owner_id": account.id(), "title": args["title"],
      "description": args["description"]
    })
    leaderboard = nil
    begin
      ActiveRecord::Transaction.run(db) do
        game.save(db)
        leaderboard = Leaderboard.new({
          "game_id": game.id(), "name": args["leaderboardName"]
        })
        leaderboard.save(db)
      end
    rescue error: ActiveRecord::ValidationError
      raise GraphQL::ExecutionError.new(error.errors().join(", "))
    end
    game.set_preloaded_association("owner", account)
    game.set_preloaded_association("leaderboards", [leaderboard])
    pheint_audit_info(context, "game.created", {
      "account_id": account.id(), "game_id": game.id(),
      "leaderboard_id": leaderboard.id()
    })
    game
  end

  def submit_score(object, args, context)
    account = context["current_account"]
    if account == nil
      raise GraphQL::ExecutionError.new("authentication required")
    end

    db = context["db"]
    leaderboard_id = args["leaderboardId"]
    if leaderboard_id is String then leaderboard_id = leaderboard_id.to_i() end
    leaderboard = Leaderboard.where({"id": leaderboard_id}).first(db)
    if leaderboard == nil
      raise GraphQL::ExecutionError.new("leaderboard not found")
    end

    player = account.player(db)
    value = args["value"]
    # Keep the comparison and write in one SQLite statement. A separate
    # read followed by #save could let two simultaneous requests overwrite
    # a higher value with a lower one between those operations.
    db.execute([
      "INSERT INTO scores (leaderboard_id, player_id, value) VALUES (?, ?, ?)",
      "ON CONFLICT(leaderboard_id, player_id) DO UPDATE SET value = excluded.value",
      "WHERE excluded.value >= scores.value"
    ].join(" "), [leaderboard.id(), player.id(), value])
    submitted_score = Score.where({
      "leaderboard_id": leaderboard.id(), "player_id": player.id()
    }).first(db)
    if value < submitted_score.value()
      raise GraphQL::ExecutionError.new(
        "score must be at least your current best of #{submitted_score.value()}")
    end
    submitted_score.set_preloaded_association("player", player)
    pheint_audit_info(context, "score.submitted", {
      "account_id": account.id(), "player_id": player.id(),
      "leaderboard_id": leaderboard.id(), "score_id": submitted_score.id(),
      "value": submitted_score.value()
    })
    submitted_score
  end

  def sign_up(object, args, context)
    email = pheint_normalize_email(args["email"])
    handle = pheint_normalize_handle(args["handle"])
    password = args["password"]
    if password.length() < 8 || password.length() > 72
      raise GraphQL::ExecutionError.new("password must be between 8 and 72 characters")
    end
    db = context["db"]
    account = Account.new({"email": email, "password_digest": nil})
    account.secure_password=(password)
    player = nil
    session = nil
    begin
      ActiveRecord::Transaction.run(db) do
        account.save(db)
        player = Player.new({"account_id": account.id(), "handle": handle})
        player.save(db)
        session = pheint_issue_session(db, account)
      end
    rescue error: ActiveRecord::ValidationError
      raise GraphQL::ExecutionError.new(error.errors().join(", "))
    end
    account.set_preloaded_association("player", player)
    pheint_audit_info(context, "authentication.account_created", {
      "account_id": account.id(), "player_id": player.id()})
    {"token": session.token(), "account": account}
  end

  def sign_in(object, args, context)
    email = pheint_normalize_email(args["email"])
    account = Account.where({"email": email}).first(context["db"])
    authenticated = if account == nil
      BCrypt.verify(args["password"], pheint_dummy_password_digest())
      false
    else
      account.authenticate(args["password"])
    end
    unless authenticated
      raise GraphQL::ExecutionError.new("invalid email or password")
    end
    session = pheint_issue_session(context["db"], account)
    pheint_audit_info(context, "authentication.session_created", {
      "account_id": account.id(), "session_id": session.id()})
    {"token": session.token(), "account": account}
  end

  def sign_out(object, args, context)
    session = context["current_session"]
    if session == nil
      raise GraphQL::ExecutionError.new("authentication required")
    end
    session_id = session.id()
    account_id = session.account_id()
    session.destroy(context["db"])
    context["current_session"] = nil
    context["current_account"] = nil
    pheint_audit_info(context, "authentication.session_destroyed", {
      "account_id": account_id, "session_id": session_id})
    true
  end
end

class PheintSchema
  def self.get()
    if @@schema == nil
      page_arguments = [
        GraphQL::Argument.new("limit", GraphQL::ScalarType.int(), 20, true),
        GraphQL::Argument.new("offset", GraphQL::ScalarType.int(), 0, true)
      ]
      player_type = GraphQL::ObjectType.new("Player")
      player_type.field("id", GraphQL::ScalarType.id().non_null(), PheintPlayerResolvers.id)
      player_type.field("handle", GraphQL::ScalarType.string().non_null(), PheintPlayerResolvers.handle)

      account_type = GraphQL::ObjectType.new("Account")
      account_type.field("id", GraphQL::ScalarType.id().non_null(), PheintAccountResolvers.id)
      account_type.field("email", GraphQL::ScalarType.string().non_null(), PheintAccountResolvers.email)
      account_type.field("player", player_type.non_null(), PheintAccountResolvers.player)

      score_type = GraphQL::ObjectType.new("Score")
      score_type.field("id", GraphQL::ScalarType.id().non_null(), PheintScoreResolvers.id)
      score_type.field("value", GraphQL::ScalarType.int().non_null(), PheintScoreResolvers.value)
      score_type.field("player", player_type.non_null(), PheintScoreResolvers.player)

      leaderboard_type = GraphQL::ObjectType.new("Leaderboard")
      leaderboard_type.field("id", GraphQL::ScalarType.id().non_null(), PheintLeaderboardResolvers.id)
      leaderboard_type.field("name", GraphQL::ScalarType.string().non_null(), PheintLeaderboardResolvers.name)
      leaderboard_type.field("scores", GraphQL::ListType.of(score_type.non_null()).non_null(),
        PheintLeaderboardResolvers.scores, page_arguments)

      game_type = GraphQL::ObjectType.new("Game")
      game_type.field("id", GraphQL::ScalarType.id().non_null(), PheintGameResolvers.id)
      game_type.field("title", GraphQL::ScalarType.string().non_null(), PheintGameResolvers.title)
      game_type.field("description", GraphQL::ScalarType.string().non_null(), PheintGameResolvers.description)
      game_type.field("owner", account_type.non_null(), PheintGameResolvers.owner)
      game_type.field("leaderboards", GraphQL::ListType.of(leaderboard_type.non_null()).non_null(), PheintGameResolvers.leaderboards)

      player_type.field("scores", GraphQL::ListType.of(score_type.non_null()).non_null(),
        PheintPlayerResolvers.scores, page_arguments)
      score_type.field("leaderboard", leaderboard_type.non_null(),
        PheintScoreResolvers.leaderboard)
      leaderboard_type.field("game", game_type.non_null(),
        PheintLeaderboardResolvers.game)

      auth_payload_type = GraphQL::ObjectType.new("AuthPayload")
      auth_payload_type.field("token", GraphQL::ScalarType.string().non_null(), PheintAuthPayloadResolvers.token)
      auth_payload_type.field("account", account_type.non_null(), PheintAuthPayloadResolvers.account)

      query = GraphQL::ObjectType.new("Query")
      query.field("apiName", GraphQL::ScalarType.string().non_null(), PheintQueryResolvers.api_name)
      query.field("environment", GraphQL::ScalarType.string().non_null(), PheintQueryResolvers.environment)
      query.field("me", account_type, PheintQueryResolvers.me)
      query.field("player", player_type, PheintQueryResolvers.player, [
        GraphQL::Argument.new("handle", GraphQL::ScalarType.string().non_null())
      ])
      query.field("leaderboard", leaderboard_type, PheintQueryResolvers.leaderboard, [
        GraphQL::Argument.new("id", GraphQL::ScalarType.id().non_null())
      ])
      query.field("game", game_type, PheintQueryResolvers.game, [
        GraphQL::Argument.new("id", GraphQL::ScalarType.id().non_null())
      ])
      query.field("myGames", GraphQL::ListType.of(game_type.non_null()).non_null(),
        PheintQueryResolvers.my_games, page_arguments)
      query.field("games", GraphQL::ListType.of(game_type.non_null()).non_null(),
        PheintQueryResolvers.games, page_arguments)

      mutation = GraphQL::ObjectType.new("Mutation")
      mutation.field("changePassword", GraphQL::ScalarType.boolean().non_null(),
        PheintMutationResolvers.change_password, [
          GraphQL::Argument.new("currentPassword", GraphQL::ScalarType.string().non_null()),
          GraphQL::Argument.new("newPassword", GraphQL::ScalarType.string().non_null())
        ])
      mutation.field("updateHandle", player_type.non_null(),
        PheintMutationResolvers.update_handle, [
          GraphQL::Argument.new("handle", GraphQL::ScalarType.string().non_null())
        ])
      mutation.field("deleteLeaderboard", GraphQL::ScalarType.boolean().non_null(),
        PheintMutationResolvers.delete_leaderboard, [
          GraphQL::Argument.new("id", GraphQL::ScalarType.id().non_null())
        ])
      mutation.field("updateLeaderboard", leaderboard_type.non_null(),
        PheintMutationResolvers.update_leaderboard, [
          GraphQL::Argument.new("id", GraphQL::ScalarType.id().non_null()),
          GraphQL::Argument.new("name", GraphQL::ScalarType.string().non_null())
        ])
      mutation.field("deleteGame", GraphQL::ScalarType.boolean().non_null(),
        PheintMutationResolvers.delete_game, [
          GraphQL::Argument.new("id", GraphQL::ScalarType.id().non_null())
        ])
      mutation.field("updateGame", game_type.non_null(), PheintMutationResolvers.update_game, [
        GraphQL::Argument.new("id", GraphQL::ScalarType.id().non_null()),
        GraphQL::Argument.new("title", GraphQL::ScalarType.string().non_null()),
        GraphQL::Argument.new("description", GraphQL::ScalarType.string().non_null())
      ])
      mutation.field("createLeaderboard", leaderboard_type.non_null(),
        PheintMutationResolvers.create_leaderboard, [
          GraphQL::Argument.new("gameId", GraphQL::ScalarType.id().non_null()),
          GraphQL::Argument.new("name", GraphQL::ScalarType.string().non_null())
        ])
      mutation.field("createGame", game_type.non_null(), PheintMutationResolvers.create_game, [
        GraphQL::Argument.new("title", GraphQL::ScalarType.string().non_null()),
        GraphQL::Argument.new("description", GraphQL::ScalarType.string().non_null()),
        GraphQL::Argument.new("leaderboardName", GraphQL::ScalarType.string().non_null())
      ])
      mutation.field("submitScore", score_type.non_null(), PheintMutationResolvers.submit_score, [
        GraphQL::Argument.new("leaderboardId", GraphQL::ScalarType.id().non_null()),
        GraphQL::Argument.new("value", GraphQL::ScalarType.int().non_null())
      ])
      mutation.field("signUp", auth_payload_type.non_null(), PheintMutationResolvers.sign_up, [
        GraphQL::Argument.new("email", GraphQL::ScalarType.string().non_null()),
        GraphQL::Argument.new("password", GraphQL::ScalarType.string().non_null()),
        GraphQL::Argument.new("handle", GraphQL::ScalarType.string().non_null())
      ])
      mutation.field("signIn", auth_payload_type.non_null(), PheintMutationResolvers.sign_in, [
        GraphQL::Argument.new("email", GraphQL::ScalarType.string().non_null()),
        GraphQL::Argument.new("password", GraphQL::ScalarType.string().non_null())
      ])
      mutation.field("signOut", GraphQL::ScalarType.boolean().non_null(), PheintMutationResolvers.sign_out)

      @@schema = GraphQL::Schema.new().query(query).mutation(mutation)
    end
    @@schema
  end
end
