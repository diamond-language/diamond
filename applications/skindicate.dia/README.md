# skindicate.dia

A repo/social app for sharing desktop and mobile reskins & themes. A
traditional server-rendered app (Dials routing, Div templates,
ActiveRecord models, cookie-based auth) -- closest in shape to
`examples/project_board`, not `applications/pheint.dia`'s GraphQL API.

The core resource is called `Skin` throughout (model, table, routes --
`/skins`, not `/reskins`); "reskin" is only the general descriptive word
for what the app is about.

## Layout

- `app.di` starts the HTTP server.
- `boot.di` loads application dependencies and source files (split out
  so `smoke_test.di` can drive `app` directly, without a real socket).
- `lib/config` owns environment configuration (`SkindicateEnvironment`).
- `lib/models` contains `User`, `Session`, `Skin`, `Tag`, and the
  `Tagging` join model.
- `lib/controllers` contains `SessionsController` (signup/login/logout)
  and `SkinsController` (browse/submit/edit/delete).
- `lib/helpers/auth.di` is DB-backed session auth wrapped in a signed
  cookie (`packages/cookies`' `SignedCookies`) -- a tampered/forged
  cookie is rejected by its signature check before any database query.
- `lib/helpers/uploads.di` saves an uploaded theme/preview file under
  `public/uploads/` with a random on-disk name.
- `lib/routes.di` and `lib/middleware.di` wire the request pipeline.
- `public/` is served by one shared `packages/rack` `StaticFiles` root
  -- both the app's own `css`/`js` and everything under `uploads/`.
- `setup_db.di` creates and seeds the database.
- `smoke_test.di` drives the whole stack end to end through the router.

## Run

```sh
cd applications/skindicate.dia
../../build/diamond setup_db.di
bash compile_views.sh
../../build/diamond app.di
```

The server listens on <http://127.0.0.1:18110> by default. `DIAMOND_ENV`
selects `development`, `test`, or `production`; `DIAMOND_DATABASE_PATH`
overrides the database file; `SKINDICATE_SESSION_SECRET` sets the
session-cookie signing key (a fixed, clearly-marked insecure default is
used otherwise, for zero-setup development only -- always set a real
secret in production). Views must be recompiled after every edit:

```sh
bash compile_views.sh
```

## Test

```sh
DIAMOND_ENV=test DIAMOND_DATABASE_PATH=skindicate_test.db ../../build/diamond setup_db.di
bash compile_views.sh
DIAMOND_ENV=test ../../build/diamond smoke_test.di
```
