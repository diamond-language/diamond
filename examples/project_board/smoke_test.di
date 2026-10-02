# Direct-dispatch smoke test: calls `app(request, context)` as the server would,
# with hand-built request Hashes, so no socket is needed. It walks the whole
# life of a user -- anonymous reads, denied writes, login, CSRF checks,
# validation, full create/update/delete flows, logout, and session expiry --
# and raises on the first thing that is not as expected. `context` is reused
# for every call, just as one gremlin worker reuses its own.
require "./boot"

# This test DELETES AND RECREATES data (and expects the seed from
# setup_db.di), so refuse to run against development or production.
if AppEnvironment.name() != "test"
  raise "smoke_test.di requires DIAMOND_ENV=test"
end

# Builds a request Hash in the shape the server delivers. `body` is a
# form-encoded string, e.g. "name=A&description=B". The cookie is the raw
# Set-Cookie value from login, which doubles as the Cookie header here.
def request(method, path, body = "", cookie = nil)
  headers = {}
  if cookie != nil
    headers["cookie"] = cookie
  end
  {"method": method, "path": path, "body": body, "headers": headers}
end

# Same thing when the cookie is always present.
def request_with_cookie(method, path, body, cookie)
  {"method": method, "path": path, "body": body, "headers": {"cookie": cookie}}
end

context = {}

# Anonymous visitors can read: the project list is public.
public_response = app(request("GET", "/projects"), context)
if public_response[0] != 200 then raise "public project index failed" end

# ...but cannot write: an anonymous POST is bounced to the login page, and
# the guard runs before CSRF is even considered.
denied_response = app(request("POST", "/projects", "name=Denied&description=Nope"), context)
if denied_response[0] != 302 || denied_response[1]["Location"] != "/login"
  raise "anonymous write was not denied"
end

# A wrong password is rejected with 401.
bad_login = app(request("POST", "/login", "email=admin%40example.com&password=wrong"), context)
if bad_login[0] != 401 then raise "bad login was not rejected" end

# A correct login (the seeded admin) redirects and sets the session cookie.
# `context["csrf_token"]` was filled in by the login action.
login = app(request("POST", "/login", "email=admin%40example.com&password=diamond123"), context)
if login[0] != 302 || login[1]["Set-Cookie"] == nil then raise "login failed" end
cookie = login[1]["Set-Cookie"]
csrf_token = context["csrf_token"]
if csrf_token == nil then raise "login did not issue a CSRF token" end

# With the cookie, the new-project form renders, and it embeds the CSRF token
# as a hidden field.
form = app(request_with_cookie("GET", "/projects/new", "", cookie), context)
if form[0] != 200 || !form[2].include?("name=\"csrf_token\"") || !form[2].include?(csrf_token)
  raise "authenticated form did not render its CSRF token"
end

# Logged in is not enough for a POST: with no token, or a wrong one, the
# write is refused with 403 and must not touch the database (1 project so
# far, from the seed).
missing_csrf = app(request_with_cookie("POST", "/projects", "name=Missing&description=Denied", cookie), context)
if missing_csrf[0] != 403 then raise "write without CSRF token was not denied" end

forged_csrf = app(request_with_cookie("POST", "/projects", "name=Forged&description=Denied&csrf_token=wrong", cookie), context)
if forged_csrf[0] != 403 then raise "write with forged CSRF token was not denied" end
if Project.all().count(Database.get(context)) != 1 then raise "a rejected CSRF write changed the database" end

# A valid token but invalid data: 422 and the error messages in the page,
# with nothing saved.
invalid_project = app(request_with_cookie("POST", "/projects", "name=&description=&csrf_token=#{csrf_token}", cookie), context)
if invalid_project[0] != 422 || !invalid_project[2].include?("name is required") || !invalid_project[2].include?("description is required")
  raise "invalid project did not render validation errors"
end
if Project.all().count(Database.get(context)) != 1 then raise "invalid project was persisted" end

# The happy path: authenticated, valid token, valid data. The `if` just
# sanity-checks that the helper attached the cookie, so a failure below
# cannot be blamed on the test harness.
created_request = request_with_cookie("POST", "/projects", "name=Instrumented&description=Authorized&csrf_token=#{csrf_token}", cookie)
if created_request["headers"]["cookie"] == nil then raise "test cookie was not attached" end
created = app(created_request, context)
if created[0] != 302 || created[1]["Location"] == "/login"
  raise "authenticated create failed"
end
if Project.all().count(Database.get(context)) != 2 then raise "authenticated create did not persist" end

# Task validation: an empty title, a nonexistent project (999) and a `done`
# of 3 (only 0 or 1 are allowed) must be reported, and nothing saved.
invalid_task = app(request_with_cookie("POST", "/tasks", "project_id=999&title=&done=3&csrf_token=#{csrf_token}", cookie), context)
if invalid_task[0] != 422 || !invalid_task[2].include?("title is required") || !invalid_task[2].include?("project must exist")
  raise "invalid task did not render validation errors"
end
if Task.all().count(Database.get(context)) != 2 then raise "invalid task was persisted" end

# Full create/update/delete flows for tasks and projects, including the
# database cascade. A function, so its locals do not leak into the script
# above.
def test_crud(context, cookie, csrf_token)
  # Reload the project created above.
  project = Project.where({"name": "Instrumented"}).first(Database.get(context))
  if project == nil then raise "created project could not be reloaded" end

  # Create a task on it and verify it is linked to the project.
  task_create = app(request_with_cookie("POST", "/tasks", "project_id=#{project.id()}&title=Write+tests&done=0&csrf_token=#{csrf_token}", cookie), context)
  if task_create[0] != 302 || task_create[1]["Location"] != "/projects/#{project.id()}"
    raise "task create failed"
  end
  task = Task.where({"title": "Write tests"}).first(Database.get(context))
  if task == nil || task.project_id() != project.id() then raise "created task association was not persisted" end

  # Update the task (rename it and mark it done).
  task_update = app(request_with_cookie("POST", "/tasks/#{task.id()}", "project_id=#{project.id()}&title=Tests+written&done=1&csrf_token=#{csrf_token}", cookie), context)
  if task_update[0] != 302 then raise "task update failed" end
  updated_task = Task.find(Database.get(context), task.id())
  if updated_task.title() != "Tests written" || !updated_task.done?() then raise "task update was not persisted" end

  # Update the project too.
  project_update = app(request_with_cookie("POST", "/projects/#{project.id()}", "name=Instrumented+board&description=Updated&csrf_token=#{csrf_token}", cookie), context)
  if project_update[0] != 302 then raise "project update failed" end
  updated_project = Project.find(Database.get(context), project.id())
  if updated_project.name() != "Instrumented board" || updated_project.description() != "Updated"
    raise "project update was not persisted"
  end

  # An anonymous visitor can see the project and its updated task.
  public_show = app(request("GET", "/projects/#{project.id()}"), context)
  if public_show[0] != 200 || !public_show[2].include?("Tests written") || !public_show[2].include?("done")
    raise "public project association view failed"
  end

  # Delete the task.
  task_delete = app(request_with_cookie("POST", "/tasks/#{task.id()}/delete", "csrf_token=#{csrf_token}", cookie), context)
  if task_delete[0] != 302 || Task.find(Database.get(context), task.id()) != nil
    raise "task delete failed"
  end

  # Cascade check: add a fresh task, delete its PROJECT, and verify the task
  # went with it. Only the schema's ON DELETE CASCADE can have done that; no
  # controller code deletes tasks.
  cascade_create = app(request_with_cookie("POST", "/tasks", "project_id=#{project.id()}&title=Cascade+me&done=0&csrf_token=#{csrf_token}", cookie), context)
  if cascade_create[0] != 302 then raise "cascade fixture task create failed" end
  cascade_task = Task.where({"title": "Cascade me"}).first(Database.get(context))
  if cascade_task == nil then raise "cascade fixture task was not persisted" end

  project_delete = app(request_with_cookie("POST", "/projects/#{project.id()}/delete", "csrf_token=#{csrf_token}", cookie), context)
  if project_delete[0] != 302 || Project.find(Database.get(context), project.id()) != nil
    raise "project delete failed"
  end
  if Task.find(Database.get(context), cascade_task.id()) != nil
    raise "project delete did not cascade to its task"
  end
end

test_crud(context, cookie, csrf_token)

# Logout: succeeds and clears the cookie.
logout_request = request_with_cookie("POST", "/logout", "csrf_token=#{csrf_token}", cookie)
logout = app(logout_request, context)
if logout[0] != 302 || logout[1]["Set-Cookie"] == nil then raise "logout failed" end

# The old cookie must now be useless: its session row is gone, so a write is
# bounced to login AND the response tells the browser to delete the stale
# cookie (Max-Age=0).
denied_again_request = request_with_cookie("POST", "/tasks", "project_id=1&title=Denied&done=0", cookie)
denied_again = app(denied_again_request, context)
if denied_again[0] != 302 || denied_again[1]["Location"] != "/login"
  raise "destroyed session still authorized a write"
end
if denied_again[1]["Set-Cookie"] == nil || !denied_again[1]["Set-Cookie"].include?("Max-Age=0")
  raise "stale session cookie was not expired"
end

# Expiry: log in again, and confirm the new session works...
second_login = app(request("POST", "/login", "email=admin%40example.com&password=diamond123"), context)
second_cookie = second_login[1]["Set-Cookie"]
if second_login[0] != 302 || second_cookie == nil then raise "second login failed" end

second_form = app(request_with_cookie("GET", "/projects/new", "", second_cookie), context)
if second_form[0] != 200 || context["current_session"] == nil then raise "second session was not loaded" end

# ...then age it by hand: set its expires_at one second into the past.
session_id = context["current_session"].id()
Database.get(context).execute("UPDATE sessions SET expires_at = ? WHERE id = ?", [Time.now().to_i() - 1, session_id])

# The expired session must be refused, the cookie cleared, and the stale row
# deleted from the database (not just ignored).
expired_form = app(request_with_cookie("GET", "/projects/new", "", second_cookie), context)
if expired_form[0] != 302 || expired_form[1]["Location"] != "/login"
  raise "expired session still authorized a protected form"
end
if expired_form[1]["Set-Cookie"] == nil || !expired_form[1]["Set-Cookie"].include?("Max-Age=0")
  raise "expired session cookie was not cleared"
end
if Session.all().count(Database.get(context)) != 0 then raise "expired session was not deleted" end

# Everything passed; as the last expression, this JSON line is the result.
JSON.stringify({"timestamp": Time.now().strftime("%Y-%m-%dT%H:%M:%S%z"), "level": "info", "tag": "project_board_smoke_test", "message": "smoke_test.passed"})
