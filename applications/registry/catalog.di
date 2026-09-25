def registry_catalog_file(path)
  file = File.open(path, "r")
  bytes = file.read()
  file.close()
  bytes
end

def registry_catalog_page(file)
  [200, {"Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store", "Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'self' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; img-src data:; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'", "X-Content-Type-Options": "nosniff"}, registry_catalog_file(file)]
end

def registry_cut_name?(name) = Regexp.new("\\A[a-z][a-z0-9_]{0,63}\\z").match?(name)

# Summary, license, links, and README of one immutable archive, read through
# the trusted verifier. Cached per digest for the life of the worker.
def registry_archive_details(context, digest)
  cache = context["registry_archive_details"]
  if cache == nil
    cache = {}
    context["registry_archive_details"] = cache
  end
  cached = cache[digest]
  if cached != nil then return cached end
  path = context["registry_store"].path(digest)
  verifier = ENV["REGISTRY_FACET"]
  details = {"readme": nil}
  verified = Process.run([verifier, "verify", path, "--sha256", digest, "--json"])
  if verified.success?()
    metadata = JSON.parse(verified.stdout())
    ["summary", "license", "homepage", "source", "documentation", "issues"].each() do |key|
      if metadata[key] is String then details[key] = metadata[key] end
    end
  end
  readme = Process.run([verifier, "verify", path, "--sha256", digest, "--readme"])
  if readme.success?() then details["readme"] = readme.stdout() end
  cache[digest] = details
  details
end

# Every served release of one cut, newest first, plus the newest unyanked
# release's (or newest release's) archive details for the show page.
def registry_cut_details(context, db, api, name)
  headers = {"Content-Type": "application/json", "Cache-Control": "no-store"}
  not_found = [404, headers, JSON.stringify({"protocol": 1, "error": "not_found", "message": "not_found"})]
  unless registry_cut_name?(name) then return not_found end
  rows = db.query("SELECT releases.version, releases.dependencies, releases.maintainers, releases.yanked, releases.sha256, releases.size, releases.created_at FROM releases JOIN cuts ON cuts.id = releases.cut_id WHERE cuts.name = ? AND releases.takedown_reason IS NULL", [name])
  if rows.length() == 0 then return not_found end
  versions = []
  rows.each() do |row|
    versions.push({"version": row["version"], "yanked": row["yanked"] == 1, "published_at": row["created_at"],
      "dependencies": JSON.parse(row["dependencies"]), "maintainers": JSON.parse(if row["maintainers"] == nil then "[]" else row["maintainers"] end),
      "sha256": row["sha256"], "size": row["size"]})
    i = versions.length() - 1
    while i > 0 && api.before?(versions[i - 1]["version"], versions[i]["version"])
      previous = versions[i - 1]
      versions[i - 1] = versions[i]
      versions[i] = previous
      i -= 1
    end
  end
  latest = versions.find() do |version| !version["yanked"] end
  if latest == nil then latest = versions[0] end
  body = {"protocol": 1, "name": name, "latest": latest["version"], "versions": versions}
  details = registry_archive_details(context, latest["sha256"])
  details.keys().each() do |key| body[key] = details[key] end
  [200, headers, JSON.stringify(body)]
end

def registry_catalog(request, db, base, api, context = {})
  path = request["path"]
  if request["method"] != "GET" then return nil end
  if path == base && base != ""
    return [302, {"Location": "#{base}/"}, ""]
  end
  if path == "#{base}/" then return registry_catalog_page("catalog.html") end
  if path == "#{base}/catalog.js" || path == "#{base}/cut.js"
    return [200, {"Content-Type": "text/javascript; charset=utf-8", "X-Content-Type-Options": "nosniff"}, registry_catalog_file(path.slice(base.length() + 1, path.length() - base.length() - 1))]
  end
  if path == "#{base}/catalog.css"
    return [200, {"Content-Type": "text/css; charset=utf-8", "X-Content-Type-Options": "nosniff"}, registry_catalog_file("catalog.css")]
  end
  cut_prefix = "#{base}/cuts/"
  if path.start_with?(cut_prefix)
    name = path.slice(cut_prefix.length(), path.length() - cut_prefix.length())
    if registry_cut_name?(name) then return registry_catalog_page("cut.html") end
    return nil
  end
  details_prefix = "#{base}/catalog/"
  if path.start_with?(details_prefix) && path.end_with?(".json")
    name = path.slice(details_prefix.length(), path.length() - details_prefix.length() - 5)
    return registry_cut_details(context, db, api, name)
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
  # One row per cut: its newest unyanked release, or its newest release when
  # every version is yanked. The cursor is the cut id.
  cuts = db.query("SELECT id FROM cuts WHERE id > ? AND EXISTS (SELECT 1 FROM releases WHERE releases.cut_id = cuts.id AND releases.takedown_reason IS NULL) ORDER BY id LIMIT 101", [after])
  more = cuts.length() > 100
  if more then cuts.pop() end
  rows = []
  if cuts.length() > 0
    releases = db.query("SELECT cuts.id, cuts.name, releases.version, releases.dependencies, releases.maintainers, releases.yanked, releases.sha256, releases.size FROM releases JOIN cuts ON cuts.id = releases.cut_id WHERE cuts.id >= ? AND cuts.id <= ? AND releases.takedown_reason IS NULL ORDER BY cuts.id", [cuts[0]["id"], cuts[cuts.length() - 1]["id"]])
    releases.each() do |release|
      last = if rows.length() > 0 then rows[rows.length() - 1] else nil end
      if last == nil || last["id"] != release["id"]
        rows.push(release)
      elsif (last["yanked"] == 1 && release["yanked"] == 0) || (last["yanked"] == release["yanked"] && api.before?(last["version"], release["version"]))
        rows[rows.length() - 1] = release
      end
    end
  end
  next_after = nil
  if more then next_after = cuts[cuts.length() - 1]["id"] end
  [200, {"Content-Type": "application/json", "Cache-Control": "no-store"}, JSON.stringify({"protocol": 1, "releases": rows, "next_after": next_after})]
end
