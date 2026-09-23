require_cut "registry"

root = ENV["REGISTRY_ROOT"]
operator = ENV["REGISTRY_OPERATOR"]
if root == nil || operator == nil then raise "REGISTRY_ROOT and REGISTRY_OPERATOR are required" end
if ARGV.length() == 0 then raise "usage: credentials.di issue SUBJECT TTL_SECONDS SCOPES REASON | revoke ID REASON | rotate ID TTL_SECONDS REASON | list" end
db = SQLite3.open(File.join(root, "registry.db"))
# Run app startup/migrations before using this operator command.
admin = Registry::Administration.new(db)
command = ARGV[0]
if command == "list" && ARGV.length() == 1
  result = db.query("SELECT id, subject, scopes, expires_at, revoked_at, created_at FROM credentials ORDER BY id")
elsif command == "issue" && ARGV.length() == 5
  ttl = ARGV[2].to_i()
  if "#{ttl}" != ARGV[2] || ttl <= 0 || ttl > 31536000 then raise "TTL must be between 1 and 31536000 seconds" end
  result = admin.issue(ARGV[1], ARGV[3].split(","), Time.now().to_i() + ttl, ARGV[4], operator)
elsif command == "revoke" && ARGV.length() == 3
  id = ARGV[1].to_i()
  if "#{id}" != ARGV[1] || id <= 0 then raise "ID must be a positive integer" end
  result = admin.revoke(id, ARGV[2], operator)
elsif command == "rotate" && ARGV.length() == 4
  ttl = ARGV[2].to_i()
  if "#{ttl}" != ARGV[2] || ttl <= 0 || ttl > 31536000 then raise "TTL must be between 1 and 31536000 seconds" end
  id = ARGV[1].to_i()
  if "#{id}" != ARGV[1] || id <= 0 then raise "ID must be a positive integer" end
  rows = db.query("SELECT * FROM credentials WHERE id = ? AND revoked_at IS NULL", [id])
  if rows.length() != 1 then raise "credential not found" end
  result = admin.issue(rows[0]["subject"], JSON.parse(rows[0]["scopes"]), Time.now().to_i() + ttl, ARGV[3], operator, id)
else
  raise "invalid credential command or arguments"
end
db.close()
JSON.stringify(result)
