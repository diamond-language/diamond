def registry_catalog_file(path)
  file = File.open(path, "r")
  bytes = file.read()
  file.close()
  bytes
end

def registry_catalog(request, db, base)
  path = request["path"]
  if request["method"] != "GET" then return nil end
  if path == base && base != ""
    return [302, {"Location": "#{base}/"}, ""]
  end
  if path == "#{base}/"
    return [200, {"Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store", "Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'unsafe-inline' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; img-src data:; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'", "X-Content-Type-Options": "nosniff"}, registry_catalog_file("catalog.html")]
  end
  if path == "#{base}/catalog.js"
    return [200, {"Content-Type": "text/javascript; charset=utf-8", "X-Content-Type-Options": "nosniff"}, registry_catalog_file("catalog.js")]
  end
  if path != "#{base}/catalog.json" && !path.start_with?("#{base}/catalog.json?after=") then return nil end
  after = 0
  if path != "#{base}/catalog.json"
    prefix = "#{base}/catalog.json?after="
    value = path.slice(prefix.length(), path.length() - prefix.length())
    if value.length() > 19 || (value.length() == 19 && value > "9223372036854775807")
      return [400, {"Content-Type": "application/json"}, JSON.stringify({"protocol": 1, "error": "invalid_request", "message": "invalid cursor"})]
    end
    after = value.to_i()
    if after < 0 || "#{after}" != value
      return [400, {"Content-Type": "application/json"}, JSON.stringify({"protocol": 1, "error": "invalid_request", "message": "invalid cursor"})]
    end
  end
  rows = db.query("SELECT releases.id, cuts.name, releases.version, releases.dependencies, releases.maintainers, releases.yanked, releases.sha256, releases.size FROM releases JOIN cuts ON cuts.id = releases.cut_id WHERE releases.id > ? AND releases.takedown_reason IS NULL ORDER BY releases.id LIMIT 101", [after])
  more = rows.length() > 100
  if more then rows.pop() end
  next_after = nil
  if more then next_after = rows[rows.length() - 1]["id"] end
  [200, {"Content-Type": "application/json", "Cache-Control": "no-store"}, JSON.stringify({"protocol": 1, "releases": rows, "next_after": next_after})]
end
