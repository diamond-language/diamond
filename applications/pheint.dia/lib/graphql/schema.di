module PheintPlayerResolvers
  module_function
  def id(player, args, context) = player.id()
  def handle(player, args, context) = player.handle()
end

module PheintScoreResolvers
  module_function
  def id(score, args, context) = score.id()
  def value(score, args, context) = score.value()
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
  def scores(leaderboard, args, context)
    if leaderboard.association_loaded?("scores")
      leaderboard.preloaded_association("scores")
    else
      leaderboard.scores(context["db"])
    end
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
  def games(object, args, context)
    planned = GraphSQL.resolve(Game.all(), context["db"], context["lookahead"],
      PheintGraphSQLMappings.games())
    if planned is ActiveRecord::Relation then planned.to_a(context["db"]) else planned end
  end
end

module PheintMutationResolvers
  module_function

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
      leaderboard_type.field("scores", GraphQL::ListType.of(score_type.non_null()).non_null(), PheintLeaderboardResolvers.scores)

      game_type = GraphQL::ObjectType.new("Game")
      game_type.field("id", GraphQL::ScalarType.id().non_null(), PheintGameResolvers.id)
      game_type.field("title", GraphQL::ScalarType.string().non_null(), PheintGameResolvers.title)
      game_type.field("description", GraphQL::ScalarType.string().non_null(), PheintGameResolvers.description)
      game_type.field("owner", account_type.non_null(), PheintGameResolvers.owner)
      game_type.field("leaderboards", GraphQL::ListType.of(leaderboard_type.non_null()).non_null(), PheintGameResolvers.leaderboards)

      auth_payload_type = GraphQL::ObjectType.new("AuthPayload")
      auth_payload_type.field("token", GraphQL::ScalarType.string().non_null(), PheintAuthPayloadResolvers.token)
      auth_payload_type.field("account", account_type.non_null(), PheintAuthPayloadResolvers.account)

      query = GraphQL::ObjectType.new("Query")
      query.field("apiName", GraphQL::ScalarType.string().non_null(), PheintQueryResolvers.api_name)
      query.field("environment", GraphQL::ScalarType.string().non_null(), PheintQueryResolvers.environment)
      query.field("me", account_type, PheintQueryResolvers.me)
      query.field("games", GraphQL::ListType.of(game_type.non_null()).non_null(),
        PheintQueryResolvers.games)

      mutation = GraphQL::ObjectType.new("Mutation")
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
