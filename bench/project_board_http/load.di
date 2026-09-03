require "../../packages/http/lib/http"

def require_status(response, expected, route)
  if response["status"] != expected
    raise "#{route}: expected HTTP #{expected}, got #{response["status"]}"
  end
  response
end

def timed_request(stats, route, method, url, headers = {}, body = "")
  start = Time.monotonic()
  response = http_request(method, url, headers, body)
  duration_ms = (Time.monotonic() - start) * 1000
  entry = stats[route]
  if entry == nil
    entry = {"count": 0, "total_ms": 0.0, "max_ms": 0.0}
    stats[route] = entry
  end
  entry["count"] = entry["count"] + 1
  entry["total_ms"] = entry["total_ms"] + duration_ms
  if duration_ms > entry["max_ms"]
    entry["max_ms"] = duration_ms
  end
  response
end

def between(value, prefix, suffix)
  start = value.index_of(prefix)
  if start == nil then raise "missing '#{prefix}' in response" end
  start = start + prefix.length()
  tail = value.slice(start, value.length())
  finish = tail.index_of(suffix)
  if finish == nil then raise "missing '#{suffix}' after '#{prefix}' in response" end
  tail.slice(0, finish)
end

def cookie_from(response)
  set_cookie = response["headers"]["set-cookie"]
  if set_cookie == nil then raise "login response omitted Set-Cookie" end
  semicolon = set_cookie.index_of(";")
  if semicolon == nil then set_cookie else set_cookie.slice(0, semicolon) end
end

def load_worker(worker_id, iterations, base_url)
  stats = {}
  require_status(timed_request(stats, "home", "GET", "#{base_url}/"), 200, "home")
  require_status(timed_request(stats, "login_form", "GET", "#{base_url}/login"), 200, "login_form")
  login = timed_request(stats, "login", "POST", "#{base_url}/login",
                        {"Content-Type": "application/x-www-form-urlencoded"},
                        "email=admin%40example.com&password=diamond123")
  require_status(login, 302, "login")
  cookie = cookie_from(login)
  headers = {"Content-Type": "application/x-www-form-urlencoded", "Cookie": cookie}
  csrf = nil

  iteration = 0
  while iteration < iterations
    suffix = "#{worker_id}-#{iteration}"

    new_project = timed_request(stats, "project_new", "GET", "#{base_url}/projects/new", {"Cookie": cookie})
    require_status(new_project, 200, "project_new")
    csrf = between(new_project["body"], "name=\"csrf_token\" value=\"", "\"")
    require_status(timed_request(stats, "projects_index", "GET", "#{base_url}/projects", {"Cookie": cookie}), 200, "projects_index")

    created = timed_request(stats, "project_create", "POST", "#{base_url}/projects", headers,
                            "name=load-#{suffix}&description=created-#{suffix}&csrf_token=#{csrf}")
    require_status(created, 302, "project_create")
    location = created["headers"]["location"]
    project_id = location.slice("/projects/".length(), location.length())

    require_status(timed_request(stats, "project_show", "GET", "#{base_url}/projects/#{project_id}", {"Cookie": cookie}), 200, "project_show")
    require_status(timed_request(stats, "project_edit", "GET", "#{base_url}/projects/#{project_id}/edit", {"Cookie": cookie}), 200, "project_edit")
    require_status(timed_request(stats, "project_update", "POST", "#{base_url}/projects/#{project_id}", headers,
                                 "name=edited-#{suffix}&description=updated-#{suffix}&csrf_token=#{csrf}"), 302, "project_update")

    require_status(timed_request(stats, "task_new", "GET", "#{base_url}/tasks/new", {"Cookie": cookie}), 200, "task_new")
    require_status(timed_request(stats, "task_create", "POST", "#{base_url}/tasks", headers,
                                 "project_id=#{project_id}&title=task-#{suffix}&done=0&csrf_token=#{csrf}"), 302, "task_create")
    with_task = timed_request(stats, "project_show", "GET", "#{base_url}/projects/#{project_id}", {"Cookie": cookie})
    require_status(with_task, 200, "project_show")
    task_row = with_task["body"].slice(with_task["body"].index_of("<td>task-#{suffix}</td>"), with_task["body"].length())
    task_id = between(task_row, "/tasks/", "/edit")
    require_status(timed_request(stats, "task_edit", "GET", "#{base_url}/tasks/#{task_id}/edit", {"Cookie": cookie}), 200, "task_edit")
    require_status(timed_request(stats, "task_update", "POST", "#{base_url}/tasks/#{task_id}", headers,
                                 "project_id=#{project_id}&title=done-#{suffix}&done=1&csrf_token=#{csrf}"), 302, "task_update")
    require_status(timed_request(stats, "task_delete", "POST", "#{base_url}/tasks/#{task_id}/delete", headers,
                                 "csrf_token=#{csrf}"), 302, "task_delete")
    require_status(timed_request(stats, "project_delete", "POST", "#{base_url}/projects/#{project_id}/delete", headers,
                                 "csrf_token=#{csrf}"), 302, "project_delete")
    iteration += 1
  end
  require_status(timed_request(stats, "logout", "POST", "#{base_url}/logout", headers,
                               "csrf_token=#{csrf}"), 302, "logout")
  stats
end

def safe_load_worker(worker_id, iterations, base_url)
  begin
    {"stats": load_worker(worker_id, iterations, base_url), "error": nil}
  rescue error
    {"stats": nil, "error": "#{error.class()}: #{error.message()}"}
  end
end

def merge_stats(combined, worker_stats)
  def merge_route(route, entry)
    aggregate = combined[route]
    if aggregate == nil
      aggregate = {"count": 0, "total_ms": 0.0, "max_ms": 0.0}
      combined[route] = aggregate
    end
    aggregate["count"] = aggregate["count"] + entry["count"]
    aggregate["total_ms"] = aggregate["total_ms"] + entry["total_ms"]
    if entry["max_ms"] > aggregate["max_ms"]
      aggregate["max_ms"] = entry["max_ms"]
    end
  end
  worker_stats.each(merge_route)
  combined
end

thread_count = if ARGV.length() > 0 then ARGV[0].to_i() else 6 end
iterations = if ARGV.length() > 1 then ARGV[1].to_i() else 100 end
base_url = if ARGV.length() > 2 then ARGV[2] else "http://127.0.0.1:19620" end
if thread_count < 1 || iterations < 1
  raise ArgumentError.new("threads and iterations must both be at least 1")
end

started = Time.monotonic()
threads = []
worker_id = 0
combined = {}
total_requests = 0
if thread_count == 1
  result = safe_load_worker(0, iterations, base_url)
  if result["error"] != nil
    puts("load worker 0 failed: #{result["error"]}")
    raise "load worker failed"
  end
  merge_stats(combined, result["stats"])
else
  while worker_id < thread_count
    threads.push(Thread.new(safe_load_worker, worker_id, iterations, base_url))
    worker_id += 1
  end
  thread_index = 0
  while thread_index < threads.length()
    result = threads[thread_index].join()
    if result["error"] != nil
      puts("load worker #{thread_index} failed: #{result["error"]}")
      raise "load worker failed"
    end
    merge_stats(combined, result["stats"])
    thread_index += 1
  end
end
elapsed_seconds = Time.monotonic() - started
def finalize_stats(combined)
  route_results = {}
  total_requests = 0
  def finish_route(route, entry)
    total_requests = total_requests + entry["count"]
    route_results[route] = {"requests": entry["count"],
                            "mean_ms": entry["total_ms"] / entry["count"],
                            "max_ms": entry["max_ms"]}
  end
  combined.each(finish_route)
  [route_results, total_requests]
end
finished = finalize_stats(combined)
route_results = finished[0]
total_requests = finished[1]

JSON.stringify({"threads": thread_count, "iterations_per_thread": iterations,
                "requests": total_requests, "elapsed_seconds": elapsed_seconds,
                "requests_per_second": total_requests / elapsed_seconds,
                "routes": route_results})
