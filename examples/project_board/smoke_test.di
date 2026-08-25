require "./boot"

def request(method, path, body = "", cookie = nil)
  headers = {}
  if cookie != nil
    headers["cookie"] = cookie
  end
  {"method": method, "path": path, "body": body, "headers": headers}
end

def request_with_cookie(method, path, body, cookie)
  {"method": method, "path": path, "body": body, "headers": {"cookie": cookie}}
end

context = {}

public_response = app(request("GET", "/projects"), context)
if public_response[0] != 200 then raise "public project index failed" end

denied_response = app(request("POST", "/projects", "name=Denied&description=Nope"), context)
if denied_response[0] != 302 || denied_response[1]["Location"] != "/login"
  raise "anonymous write was not denied"
end

bad_login = app(request("POST", "/login", "email=admin%40example.com&password=wrong"), context)
if bad_login[0] != 401 then raise "bad login was not rejected" end

login = app(request("POST", "/login", "email=admin%40example.com&password=diamond123"), context)
if login[0] != 302 || login[1]["Set-Cookie"] == nil then raise "login failed" end
cookie = login[1]["Set-Cookie"]
csrf_token = context["csrf_token"]
if csrf_token == nil then raise "login did not issue a CSRF token" end

form = app(request_with_cookie("GET", "/projects/new", "", cookie), context)
if form[0] != 200 || !form[2].include?("name=\"csrf_token\"") || !form[2].include?(csrf_token)
  raise "authenticated form did not render its CSRF token"
end

missing_csrf = app(request_with_cookie("POST", "/projects", "name=Missing&description=Denied", cookie), context)
if missing_csrf[0] != 403 then raise "write without CSRF token was not denied" end

forged_csrf = app(request_with_cookie("POST", "/projects", "name=Forged&description=Denied&csrf_token=wrong", cookie), context)
if forged_csrf[0] != 403 then raise "write with forged CSRF token was not denied" end
if Project.all().count(Database.get(context)) != 1 then raise "a rejected CSRF write changed the database" end

invalid_project = app(request_with_cookie("POST", "/projects", "name=&description=&csrf_token=#{csrf_token}", cookie), context)
if invalid_project[0] != 422 || !invalid_project[2].include?("name is required") || !invalid_project[2].include?("description is required")
  raise "invalid project did not render validation errors"
end
if Project.all().count(Database.get(context)) != 1 then raise "invalid project was persisted" end

created_request = request_with_cookie("POST", "/projects", "name=Instrumented&description=Authorized&csrf_token=#{csrf_token}", cookie)
if created_request["headers"]["cookie"] == nil then raise "test cookie was not attached" end
created = app(created_request, context)
if created[0] != 302 || created[1]["Location"] == "/login"
  raise "authenticated create failed"
end
if Project.all().count(Database.get(context)) != 2 then raise "authenticated create did not persist" end

invalid_task = app(request_with_cookie("POST", "/tasks", "project_id=999&title=&done=3&csrf_token=#{csrf_token}", cookie), context)
if invalid_task[0] != 422 || !invalid_task[2].include?("title is required") || !invalid_task[2].include?("project must exist")
  raise "invalid task did not render validation errors"
end
if Task.all().count(Database.get(context)) != 2 then raise "invalid task was persisted" end

def test_crud(context, cookie, csrf_token)
  project = Project.where({"name": "Instrumented"}).first(Database.get(context))
  if project == nil then raise "created project could not be reloaded" end

  task_create = app(request_with_cookie("POST", "/tasks", "project_id=#{project.id()}&title=Write+tests&done=0&csrf_token=#{csrf_token}", cookie), context)
  if task_create[0] != 302 || task_create[1]["Location"] != "/projects/#{project.id()}"
    raise "task create failed"
  end
  task = Task.where({"title": "Write tests"}).first(Database.get(context))
  if task == nil || task.project_id() != project.id() then raise "created task association was not persisted" end

  task_update = app(request_with_cookie("POST", "/tasks/#{task.id()}", "project_id=#{project.id()}&title=Tests+written&done=1&csrf_token=#{csrf_token}", cookie), context)
  if task_update[0] != 302 then raise "task update failed" end
  updated_task = Task.find(Database.get(context), task.id())
  if updated_task.title() != "Tests written" || !updated_task.done?() then raise "task update was not persisted" end

  project_update = app(request_with_cookie("POST", "/projects/#{project.id()}", "name=Instrumented+board&description=Updated&csrf_token=#{csrf_token}", cookie), context)
  if project_update[0] != 302 then raise "project update failed" end
  updated_project = Project.find(Database.get(context), project.id())
  if updated_project.name() != "Instrumented board" || updated_project.description() != "Updated"
    raise "project update was not persisted"
  end

  public_show = app(request("GET", "/projects/#{project.id()}"), context)
  if public_show[0] != 200 || !public_show[2].include?("Tests written") || !public_show[2].include?("done")
    raise "public project association view failed"
  end

  task_delete = app(request_with_cookie("POST", "/tasks/#{task.id()}/delete", "csrf_token=#{csrf_token}", cookie), context)
  if task_delete[0] != 302 || Task.find(Database.get(context), task.id()) != nil
    raise "task delete failed"
  end

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

logout_request = request_with_cookie("POST", "/logout", "csrf_token=#{csrf_token}", cookie)
logout = app(logout_request, context)
if logout[0] != 302 || logout[1]["Set-Cookie"] == nil then raise "logout failed" end

denied_again_request = request_with_cookie("POST", "/tasks", "project_id=1&title=Denied&done=0", cookie)
denied_again = app(denied_again_request, context)
if denied_again[0] != 302 || denied_again[1]["Location"] != "/login"
  raise "destroyed session still authorized a write"
end
if denied_again[1]["Set-Cookie"] == nil || !denied_again[1]["Set-Cookie"].include?("Max-Age=0")
  raise "stale session cookie was not expired"
end

second_login = app(request("POST", "/login", "email=admin%40example.com&password=diamond123"), context)
second_cookie = second_login[1]["Set-Cookie"]
if second_login[0] != 302 || second_cookie == nil then raise "second login failed" end

second_form = app(request_with_cookie("GET", "/projects/new", "", second_cookie), context)
if second_form[0] != 200 || context["current_session"] == nil then raise "second session was not loaded" end
session_id = context["current_session"].id()
Database.get(context).execute("UPDATE sessions SET expires_at = ? WHERE id = ?", [Time.now().to_i() - 1, session_id])

expired_form = app(request_with_cookie("GET", "/projects/new", "", second_cookie), context)
if expired_form[0] != 302 || expired_form[1]["Location"] != "/login"
  raise "expired session still authorized a protected form"
end
if expired_form[1]["Set-Cookie"] == nil || !expired_form[1]["Set-Cookie"].include?("Max-Age=0")
  raise "expired session cookie was not cleared"
end
if Session.all().count(Database.get(context)) != 0 then raise "expired session was not deleted" end

JSON.stringify({"timestamp": Time.now().strftime("%Y-%m-%dT%H:%M:%S%z"), "level": "info", "tag": "project_board_smoke_test", "message": "smoke_test.passed"})
