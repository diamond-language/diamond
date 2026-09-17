# CLI entry point: diamond packages/active_record/bin/generate_migration.di <output_dir> <name>
#
# Creates <output_dir>/<timestamp>_<name>.di with up/down stubs and a
# <name>_migration() function, matching ActiveRecord::Migrator's own
# file convention exactly (README.md's "Migrations" section) -- the
# generated file needs no editing to be `require`d and listed in a
# project's own migrate.di, just filled in.
#
# `timestamp` is `Time.utc_now()` formatted as `YYYYMMDDHHMMSS`, the
# same shape the README's own example ("20260101120000") already uses.
# Nothing in Migrator itself sorts or compares this string (README:
# "nothing here sorts or compares version values at all... a caller's
# own Array order is the only order that matters, timestamp-shaped or
# not") -- using a real timestamp is purely so two migrations generated
# minutes apart sort correctly if a project's own migrate.di ever lists
# `require`s in filename order rather than tracking order by hand.

if ARGV.length() < 2
  puts("usage: diamond packages/active_record/bin/generate_migration.di <output_dir> <name>")
  exit(1)
end

output_dir = ARGV[0]
name = ARGV[1]
timestamp = Time.utc_now().strftime("%Y%m%d%H%M%S")
path = "#{output_dir}/#{timestamp}_#{name}.di"

content = "def #{name}_up(db)\n" +
  "  # db.execute(\"...\")\n" +
  "end\n\n" +
  "def #{name}_down(db)\n" +
  "  # db.execute(\"...\")\n" +
  "end\n\n" +
  "def #{name}_migration() = {\n" +
  "  \"version\": \"#{timestamp}\", \"up\": #{name}_up, \"down\": #{name}_down\n" +
  "}\n"

file = File.open(path, "w")
file.write(content)
file.close()
puts("created #{path}")
