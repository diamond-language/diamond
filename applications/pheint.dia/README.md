# pheint.dia

A GraphQL-only Diamond API built on Gremlin, Rack, Dials, `graphql`, and the
structured logger. The browser application will be a separate React frontend;
this project does not render HTML or own frontend assets.

## Layout

- `app.di` starts the HTTP server.
- `boot.di` loads application dependencies and source files.
- `lib/config` owns environment configuration.
- `lib/controllers` contains JSON and GraphQL request actions.
- `lib/graphql` contains the schema and resolvers.
- `lib/models` contains `Account`, `Player`, and server-side `Session` models.
- `lib/helpers` contains shared application helpers.
- `lib/routes.di` and `lib/middleware.di` wire the request pipeline.

## Run

```sh
cd applications/pheint.dia
../../build/diamond app.di
```

The server listens on <http://127.0.0.1:18100> by default. `PORT` overrides
the port, `DIAMOND_ENV` selects `development`, `test`, or `production`, and
`LOG_LEVEL` overrides the environment log threshold. Logs are NDJSON and can
be formatted during development with `packages/log_viewer`:

```sh
../../build/diamond app.di | DIAMOND_BIN=../../build/diamond ../../packages/log_viewer/bin/diamond-log
```

The API exposes:

- `POST /graphql` with the standard JSON `query`, `variables`, and
  `operationName` request fields;
- `GET /health` for service health;
- `GET /` for JSON service metadata.

## Database

`DIAMOND_ENV` isolates `pheint_development.db`, `pheint_test.db`, and
`pheint_production.db`; `DIAMOND_DATABASE_PATH` overrides the selected path.
Initialize the selected database with:

```sh
../../build/diamond setup_db.di
```

This command recreates and seeds the selected database schema. Accounts store normalized
email and a bcrypt password digest. Each account has one player with a unique
handle, and authentication tokens are persisted as expiring sessions.

The game domain is:

- an account owns many games;
- a game belongs to its owner and has a title, description, and many
  leaderboards;
- a leaderboard belongs to a game and has many scores;
- a score belongs to a player and a leaderboard and stores an integer value;
- `[leaderboard_id, player_id]` is unique, enforcing one score per user on
  each leaderboard without duplicating the account ID on the score row.

The seed creates `demo@pheint.dia` / `@demo`, two games (`Asteroid Run` and
`Cipher Sprint`), leaderboards for both, and an initial Asteroid Run score.
The development-only demo password is `diamond123`; test uses the same fixture
at a reduced bcrypt cost, while non-test seeds use cost 12.

## Authentication

The schema exposes the conventional authentication flow:

```graphql
mutation SignUp {
  signUp(email: "ada@example.com", password: "correct horse", handle: "@ada") {
    token
    account { id email player { id handle } }
  }
}

mutation SignIn {
  signIn(email: "ada@example.com", password: "correct horse") {
    token
    account { id email player { handle } }
  }
}

query CurrentAccount {
  me { id email player { handle } }
}

mutation SignOut { signOut }

mutation SubmitScore {
  submitScore(leaderboardId: 1, value: 140000) {
    id
    value
    player { handle }
  }
}

query Games {
  games {
    id
    title
    description
    owner { id email player { handle } }
    leaderboards {
      id
      name
      scores { value player { handle } }
    }
  }
}
```

Send the returned opaque token as `Authorization: Bearer <token>` for `me` and
`signOut`, and `submitScore`. Score submission creates a player's first score
on a leaderboard and updates that same row when the new value is higher. An
equal value is an idempotent success; a value below the player's current best
is rejected without changing the stored score. Sessions expire after 30 days
and are deleted when an expired token is presented. Signup lowercases email
and handle, accepts the handle with or without a leading `@`, and creates the
account, player, and initial session in one transaction. Failed player/account
validation rolls the entire signup
back. Passwords must be 8–72 characters; only bcrypt digests are persisted,
and neither password digests nor session records are exposed by the schema.

The public `games` query is planned through GraphSQL. Its lookahead selects
only requested columns and recursively batch-loads requested owner,
leaderboard, score, and player associations.

Run the direct-dispatch smoke test with:

```sh
DIAMOND_ENV=test ../../build/diamond smoke_test.di
```
