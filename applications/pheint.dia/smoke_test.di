require "./boot"

if PheintEnvironment.name() != "test"
  raise "smoke_test.di requires DIAMOND_ENV=test"
end

def smoke_request(path)
  {"method": "GET", "path": path, "body": "", "headers": {}}
end

context = {"log_level": "off"}
home = app(smoke_request("/"), context)
if home[0] != 200 || !home[2].include?("pheint.dia")
  raise "home page smoke test failed"
end

health = app(smoke_request("/health"), context)
if health[0] != 200 || health[1]["Content-Type"] != "application/json" ||
   !health[2].include?("\"status\":\"ok\"")
  raise "health endpoint smoke test failed"
end

missing = app(smoke_request("/missing"), context)
if missing[0] != 404
  raise "missing route smoke test failed"
end

puts("pheint.dia smoke ok")
