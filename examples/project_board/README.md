# Authenticated project board

This example is a basic CRUD app whose reads are public and whose writes are protected by cookie-based authentication. It demonstrates four `ActiveRecord::Model` classes and explicit associations:

- a project `has_many` tasks and a task `belongs_to` its project;
- a user `has_many` sessions and a session `belongs_to` its user.

Passwords are stored as bcrypt digests through `ActiveRecord::Model#secure_password=`/`#authenticate`. Successful login creates independent 256-bit random session and CSRF tokens. The opaque session token is persisted in SQLite with an eight-hour absolute expiry and sent in an `HttpOnly`, `SameSite=Lax` cookie with the same lifetime. Expired sessions are rejected and deleted server-side. The CSRF token is embedded in authenticated forms and checked without content-dependent early exit on every state-changing route. Logout deletes the server-side session and expires the cookie.

Rack middleware loads the current session into each request's context. Authorization is declared beside each protected route using Dials route filters: anonymous users may access `/`, project indexes, and project detail pages, while every new/edit/create/update/delete route short-circuits before its controller. The UI also hides write controls from anonymous visitors, but that is only presentation—the route filter is the security boundary.

Projects and tasks use `ActiveRecord::Validators` at the repository boundary. Names, descriptions, and titles are required and length-limited; task status is constrained to its two valid values, and every task must reference an existing project. Controllers rescue `ActiveRecord::ValidationError` and return an HTTP 422 form preserving the submitted values and listing every error.

Every request receives a random request ID and is logged at start and completion with status and duration. Debug logs cover controller reads and authorization decisions; info logs cover database/model initialization, successful authentication, sessions, and every mutation; warnings cover denied writes, failed logins, stale cookies, and missing delete targets.

The app's instrumented database adapter also captures every ActiveRecord/Arel query and write with a separate query ID, generated SQL, operation, row or affected count, and elapsed milliseconds. SQL retains placeholders and logs only the number of bound parameters: passwords, password digests, session tokens, and other bound values are never logged.

## Run

```sh
cd examples/project_board
bash compile_views.sh
../../build/diamond setup_db.di
../../build/diamond app.di
```

Run the direct-dispatch smoke test (public read, denied anonymous write, failed login, missing/forged CSRF rejection, validation failures without persistence, successful login/write/logout, rejected stale session, and expired-session deletion) with:

```sh
../../build/diamond setup_db.di
../../build/diamond smoke_test.di
```

Open <http://127.0.0.1:18081/> and sign in with:

```text
admin@example.com
diamond123
```

For a quick command-line check:

```sh
curl -i http://127.0.0.1:18081/projects
curl -i -X POST http://127.0.0.1:18081/projects -d 'name=Nope&description=Anonymous'
curl -i -c cookies.txt -d 'email=admin%40example.com&password=diamond123' http://127.0.0.1:18081/login
# Open a form in a browser for authenticated writes; each form carries its
# session's CSRF token in addition to the cookie.
```

`setup_db.di` is intentionally destructive to this example's local database: rerun it whenever you want the seed state back.
