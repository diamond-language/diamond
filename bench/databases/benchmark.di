require "../../packages/database_config/lib/database_config"

if ARGV.length() < 5
  raise ArgumentError.new(
    "usage: benchmark.di CONFIG PROFILE ROWS WARMUP_ITERATIONS ITERATIONS")
end

config_path = ARGV[0]
profile = ARGV[1]
row_count = ARGV[2].to_i()
warmup_iterations = ARGV[3].to_i()
iterations = ARGV[4].to_i()

config = DatabaseConfig.load(config_path, profile)

connection_started = Time.monotonic()
db = DatabaseConfig.open(config)
connection_seconds = Time.monotonic() - connection_started

db.execute("DROP TABLE IF EXISTS diamond_benchmark_items")
db.execute("CREATE TABLE diamond_benchmark_items (id INTEGER PRIMARY KEY, name VARCHAR(100) NOT NULL, value INTEGER NOT NULL)")

seed_started = Time.monotonic()
index = 1
while index <= row_count
  db.execute("INSERT INTO diamond_benchmark_items (id, name, value) VALUES (?, ?, ?)",
    [index, "item-#{index}", index * 3])
  index += 1
end
seed_seconds = Time.monotonic() - seed_started

# Exercise the same prepared query shapes before measuring each server. The
# native drivers prepare each call independently, so this warms server caches
# and data pages rather than retaining a client-side prepared statement.
index = 0
while index < warmup_iterations
  id = (index % row_count) + 1
  db.query("SELECT value FROM diamond_benchmark_items WHERE id = ?", [id])
  index += 1
end

checksum = 0
started = Time.monotonic()
index = 0
while index < iterations
  id = (index % row_count) + 1
  checksum += db.query(
    "SELECT value FROM diamond_benchmark_items WHERE id = ?", [id])[0]["value"]
  index += 1
end
point_read_seconds = Time.monotonic() - started

range_checksum = 0
started = Time.monotonic()
index = 0
while index < iterations
  floor = index % row_count
  rows = db.query(
    "SELECT id, value FROM diamond_benchmark_items WHERE value >= ? ORDER BY value LIMIT 20",
    [floor * 3])
  range_checksum += rows.length()
  index += 1
end
range_read_seconds = Time.monotonic() - started

started = Time.monotonic()
index = 0
while index < iterations
  id = (index % row_count) + 1
  db.execute("UPDATE diamond_benchmark_items SET value = value + 1 WHERE id = ?", [id])
  index += 1
end
update_seconds = Time.monotonic() - started

db.close()

puts(JSON.stringify({
  "engine": profile,
  "adapter": DatabaseConfig.adapter(config),
  "rows": row_count,
  "warmup_iterations": warmup_iterations,
  "iterations": iterations,
  "connection_seconds": connection_seconds,
  "seed_seconds": seed_seconds,
  "point_read_seconds": point_read_seconds,
  "point_reads_per_second": iterations / point_read_seconds,
  "range_read_seconds": range_read_seconds,
  "range_reads_per_second": iterations / range_read_seconds,
  "update_seconds": update_seconds,
  "updates_per_second": iterations / update_seconds,
  "checksum": checksum,
  "range_checksum": range_checksum
}))
exit(0)
