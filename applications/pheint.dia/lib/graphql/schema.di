module PheintProfileResolvers
  module_function
  def id(profile, args, context) = profile.id()
  def handle(profile, args, context) = profile.handle()
end

module PheintAccountResolvers
  module_function
  def id(account, args, context) = account.id()
  def email(account, args, context) = account.email()
  def profile(account, args, context) = account.profile(context["db"])
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
end

module PheintMutationResolvers
  module_function

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
    profile = nil
    session = nil
    begin
      ActiveRecord::Transaction.run(db) do
        account.save(db)
        profile = Profile.new({"account_id": account.id(), "handle": handle})
        profile.save(db)
        session = pheint_issue_session(db, account)
      end
    rescue error: ActiveRecord::ValidationError
      raise GraphQL::ExecutionError.new(error.errors().join(", "))
    end
    account.set_preloaded_association("profile", profile)
    pheint_audit_info(context, "authentication.account_created", {
      "account_id": account.id(), "profile_id": profile.id()})
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
      profile_type = GraphQL::ObjectType.new("Profile")
      profile_type.field("id", GraphQL::ScalarType.id().non_null(), PheintProfileResolvers.id)
      profile_type.field("handle", GraphQL::ScalarType.string().non_null(), PheintProfileResolvers.handle)

      account_type = GraphQL::ObjectType.new("Account")
      account_type.field("id", GraphQL::ScalarType.id().non_null(), PheintAccountResolvers.id)
      account_type.field("email", GraphQL::ScalarType.string().non_null(), PheintAccountResolvers.email)
      account_type.field("profile", profile_type.non_null(), PheintAccountResolvers.profile)

      auth_payload_type = GraphQL::ObjectType.new("AuthPayload")
      auth_payload_type.field("token", GraphQL::ScalarType.string().non_null(), PheintAuthPayloadResolvers.token)
      auth_payload_type.field("account", account_type.non_null(), PheintAuthPayloadResolvers.account)

      query = GraphQL::ObjectType.new("Query")
      query.field("apiName", GraphQL::ScalarType.string().non_null(), PheintQueryResolvers.api_name)
      query.field("environment", GraphQL::ScalarType.string().non_null(), PheintQueryResolvers.environment)
      query.field("me", account_type, PheintQueryResolvers.me)

      mutation = GraphQL::ObjectType.new("Mutation")
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
