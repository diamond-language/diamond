# Build a deterministic registry seed in a new directory; never publish it.
#
#   diamond prepare_registry_seed.di DESTINATION [--selection FILE] [--facet FILE] [--expect FILE]
#
# --expect requires an exact match to a reviewed inventory. The destination is created exclusively
# (an existing directory is never overwritten) and removed again if anything fails.
require "lib/files"

root = File.dirname(__dir__)

def facet_output(facet, args)
  result = Process.run([facet] + args)
  unless result.success?()
    message = result.stderr().strip()
    raise ArgumentError.new(message == "" ? "facet command failed" : message)
  end
  result.stdout()
end

def valid_name?(name)
  Regexp.new("\\A[a-z][a-z0-9_]*\\z").match?(name)
end

def visit_cut(name, entries, visiting, visited, ordered)
  raise ArgumentError.new("dependency missing from seed: " + name) unless entries.key?(name)
  raise ArgumentError.new("dependency cycle at: " + name) if visiting.include?(name)
  return nil if visited.include?(name)
  visiting.push(name)
  entries[name]["dependencies"].keys().sort().each() do |dependency|
    visit_cut(dependency, entries, visiting, visited, ordered)
  end
  visiting.pop()
  visited.push(name)
  ordered.push(entries[name])
  nil
end

def prepare(root, destination, selection_path, facet, expected_path)
  selected = JSON.parse(File.read(selection_path))
  unless selected.is_a?(Hash) && selected.length() > 0
    raise ArgumentError.new("selection must map cut names to exact versions")
  end
  selected.each() do |name, version|
    unless valid_name?(name) && version.is_a?(String)
      raise ArgumentError.new("invalid selected name or version")
    end
  end
  # mkdir is exclusive: an existing directory is never overwritten.
  Dir.mkdir(destination, 0o700)
  begin
    entries = {}
    selected.keys().sort().each() do |name|
      archive = destination + "/" + name + ".tar"
      facet_output(facet, ["pack", root + "/packages/" + name, archive])
      digest = Digest.sha256(File.read(archive))
      metadata = JSON.parse(facet_output(facet, ["verify", archive, "--sha256", digest, "--json"]))
      if metadata["name"] != name || metadata["version"] != selected[name]
        raise ArgumentError.new("selected identity changed: " + name)
      end
      entry = {}
      ["name", "version", "dependencies", "sha256", "size"].each() do |key|
        entry[key] = metadata[key]
      end
      entry["archive"] = name + ".tar"
      entries[name] = entry
    end

    # Dependencies before their dependents, in name order otherwise.
    ordered = []
    visiting = []
    visited = []
    entries.keys().sort().each() do |name|
      visit_cut(name, entries, visiting, visited, ordered)
    end
    inventory = {"format": 1, "cuts": ordered}
    if expected_path != nil && inventory != JSON.parse(File.read(expected_path))
      raise ArgumentError.new("seed differs from reviewed inventory; review changes before updating it")
    end
    File.write(destination + "/inventory.json", JSON.stringify(inventory, {"indent": 2, "sort_keys": true}) + "\n")
    inventory
  rescue e: StandardError
    remove_tree(destination)
    raise e
  end
end

destination = nil
selection = root + "/applications/registry/launch-cuts.json"
facet = root + "/build/facet"
expected = nil
index = 0
while index < ARGV.length()
  arg = ARGV[index]
  if arg == "--selection" || arg == "--facet" || arg == "--expect"
    index += 1
    if index >= ARGV.length()
      warn("#{arg} needs a value")
      exit(2)
    end
    selection = ARGV[index] if arg == "--selection"
    facet = ARGV[index] if arg == "--facet"
    expected = ARGV[index] if arg == "--expect"
  elsif destination == nil && !arg.start_with?("--")
    destination = arg
  else
    warn("unexpected argument #{arg}")
    exit(2)
  end
  index += 1
end
if destination == nil
  warn("usage: prepare_registry_seed.di DESTINATION [--selection FILE] [--facet FILE] [--expect FILE]")
  exit(2)
end

begin
  inventory = prepare(root, destination, selection, facet, expected)
  puts("prepared #{inventory["cuts"].length()} verified cuts in #{destination}")
rescue e: StandardError
  warn("seed preparation failed: #{e.message()}")
  exit(1)
end
exit(0)
