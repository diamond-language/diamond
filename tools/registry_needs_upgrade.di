# Say whether the live registry needs upgrading to a given revision.
#
#   diamond registry_needs_upgrade.di [--deployed REVISION] [--host HOST] [--target REVISION]
#
# The registry ships as one bundle built from a single repo revision, but most
# releases change nothing it runs. This compares the revision currently deployed
# (read from the server, or given with --deployed) against a target revision
# (default HEAD) and classifies what changed:
#
#   REQUIRED  something the registry itself executes changed: its app files, a cut
#             it loads (the dependency closure of its `require_cut` lines), or
#             the facet it publishes/installs with.  exit 20
#   OPTIONAL  only the Diamond runtime changed (src/, lib/, reginold/, Makefile).
#             The registry keeps working on its current interpreter; upgrade for a
#             crash or security fix that affects it, otherwise wait for the next
#             REQUIRED change.  exit 10
#   NONE      nothing relevant changed.  exit 0
#
# Changes to the bundled seed (docs/ and launch-cuts.json) and to deploy/ templates
# never require an upgrade: new cut versions are published through the API, and
# deploy/ files are reference configs applied by hand.

root = File.dirname(__dir__)
RUNTIME_PATHS = ["src/", "lib/", "reginold/", "Makefile"]
FACET_PATHS = ["tools/facet.c", "tools/semver.c", "tools/semver.h"]

def fail_with(message)
  warn(message)
  exit(1)
end

def git(root, args)
  # core.quotepath=off keeps a non-ASCII path whole in `diff --name-only`; git would otherwise print it quoted and escaped, which no prefix test matches.
  result = Process.run(["git", "-C", root, "-c", "core.quotepath=off"] + args)
  raise ArgumentError.new(result.stderr().strip()) unless result.success?()
  result.stdout()
end

# The registry app files the bundle copies, read from the build script so the two cannot
# drift. Falls back to the whole directory if that fails.
def shipped_app_files(root)
  script = File.read(root + "/tools/build_registry_vm.sh")
  found = script.scan(Regexp.new("cp applications/registry/\\{[^}]+\\} payload/app/"))
  return ["applications/registry/"] if found.length() == 0
  list = found[0].split("{")[1].split("}")[0]
  list.split(",").map() do |name| "applications/registry/" + name end
end

# Every cut the registry loads: its require_cut roots plus their dependencies, read from each
# packages/<name>/diamond.cut.
def cut_closure(root, roots)
  seen = []
  queue = roots.dup()
  while queue.length() > 0
    name = queue.pop()
    next if seen.include?(name)
    seen.push(name)
    manifest = root + "/packages/" + name + "/diamond.cut"
    if File.exist?(manifest)
      dependencies = JSON.parse(File.read(manifest))["dependencies"]
      if dependencies != nil
        dependencies.keys().each() do |dependency|
          queue.push(dependency)
        end
      end
    end
  end
  seen.sort()
end

def registry_roots(root)
  names = []
  Dir.entries(root + "/applications/registry").each() do |entry|
    next unless entry.end_with?(".di")
    File.read(root + "/applications/registry/" + entry).scan(Regexp.new("require_cut\\s+\"[a-z_0-9]+\"")).each() do |match|
      name = match.split("\"")[1]
      names.push(name) unless names.include?(name)
    end
  end
  names.sort()
end

def deployed_revision(host)
  target = Process.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host,
    "readlink /opt/diamond-registry/current"])
  unless target.success?()
    fail_with("cannot read the deployed revision from #{host}: #{target.stderr().strip()}")
  end
  revision = File.basename(target.stdout().strip())
  unless Regexp.new("\\A[0-9a-f]{40}\\z").match?(revision)
    fail_with("unexpected deployed revision #{revision.inspect()}")
  end
  revision
end

# Docs and tests inside a package are not executed by the registry. Some packages keep their
# tests beside the sources (packages/registry/http_test.di), not in test/.
def inert?(path)
  Regexp.new("\\Apackages/[^/]+/(README\\.md|LICENSE|test\\.sh|test/|[^/]+_test\\.(py|di|sh))").match?(path)
end

def hits(changed, prefixes)
  changed.select() do |path|
    !inert?(path) && prefixes.any?() do |prefix|
      path == prefix || (prefix.end_with?("/") && path.start_with?(prefix))
    end
  end
end

deployed = nil
host = "root@dilang.tech"
target_name = "HEAD"
index = 0
while index < ARGV.length()
  arg = ARGV[index]
  index += 1
  if arg == "--deployed" || arg == "--host" || arg == "--target"
    fail_with("#{arg} needs a value") if index >= ARGV.length()
    deployed = ARGV[index] if arg == "--deployed"
    host = ARGV[index] if arg == "--host"
    target_name = ARGV[index] if arg == "--target"
    fail_with("#{arg} must not start with a dash") if ARGV[index].start_with?("-")
    index += 1
  else
    fail_with("unexpected argument #{arg}")
  end
end

deployed = deployed_revision(host) if deployed == nil
begin
  git(root, ["cat-file", "-e", deployed + "^{commit}"])
rescue e: ArgumentError
  fail_with("deployed revision #{deployed[0, 12]} is not in this checkout (git fetch?)")
end
target = git(root, ["rev-parse", target_name + "^{commit}"]).strip()
changed = git(root, ["diff", "--name-only", deployed, target]).split("\n").select() do |line| line != "" end

cuts = cut_closure(root, registry_roots(root))
required_prefixes = shipped_app_files(root) + FACET_PATHS + cuts.map() do |name| "packages/#{name}/" end
required = hits(changed, required_prefixes)
runtime = hits(changed, RUNTIME_PATHS)
puts("deployed #{deployed[0, 12]} -> target #{target[0, 12]}: #{changed.length()} files changed")
puts("registry loads cuts: #{cuts.join(", ")}")
if required.length() > 0
  puts("\nREQUIRED -- the registry's own code changed:")
  required.each() do |path|
    puts("  #{path}")
  end
  puts("(plus #{runtime.length()} runtime file(s))") if runtime.length() > 0
  exit(20)
end
if runtime.length() > 0
  puts("\nOPTIONAL -- only the Diamond runtime changed (#{runtime.length()} file(s)):")
  runtime[0, 15].each() do |path|
    puts("  #{path}")
  end
  subjects = git(root, ["log", "--no-merges", "--format=  %h %s", "#{deployed}..#{target}", "--"] + RUNTIME_PATHS).split("\n")
  puts("runtime commits since deploy:")
  puts(subjects[0, 15].join("\n"))
  puts("\nUpgrade only for a crash/security fix that affects the registry; otherwise wait for the next REQUIRED change.")
  exit(10)
end
puts("\nNONE -- nothing the registry runs has changed.")
exit(0)
