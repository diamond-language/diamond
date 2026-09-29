# plugins: a host that runs untrusted Diamond scripts as sandboxed
# subprocesses, and a tour of the sandbox/resource-limit features that
# make that possible: docs/sandbox.md's `--sandbox`, its per-capability
# DIAMOND_SANDBOX_ALLOW allow-list, and DIAMOND_MAX_INSTRUCTIONS/
# DIAMOND_MAX_WALL_MILLISECONDS.
#
# Each plugin runs in its OWN process (Process.run), not just its own
# DiamondVm -- real defense in depth, on top of --sandbox itself: a plugin
# that somehow escaped the interpreter's own checks would still be a
# separate OS process with nothing but the pipe back to this one. The
# manifest (manifest.json) is the host's policy: it decides per plugin
# which capabilities to grant, by setting DIAMOND_SANDBOX_ALLOW only for
# that child. A plugin never gets a say in its own trust level.

struct PluginResult(name: String, status: String, exit_code: Int, note: String)
end

def build_argv(diamond_bin, spec)
  env_args = ["DIAMOND_MAX_INSTRUCTIONS=#{spec["max_instructions"]}",
              "DIAMOND_MAX_WALL_MILLISECONDS=#{spec["max_wall_ms"]}"]
  allow = spec["allow"]
  if !allow.empty?()
    env_args = env_args + ["DIAMOND_SANDBOX_ALLOW=#{allow.join(",")}"]
  end
  argv = ["env"] + env_args + [diamond_bin, "--sandbox", spec["path"]]
  arg = spec["arg"]
  argv = arg.empty?() ? argv : argv + [arg]
  argv
end

# Classifies a finished child by inspecting its exit code and stderr --
# the same evidence a real operator has, since a crashed child's raised
# exception never crosses the process boundary as an object, only as the
# text `diamond`'s own uncaught-error reporter printed.
def classify(result)
  if result.success?()
    return PluginResult.new("", "ok", 0, result.stdout().strip())
  end
  stderr = result.stderr()
  if stderr.include?("sandbox denies")
    PluginResult.new("", "sandbox_denied", result.exit_code(), stderr.strip())
  elsif stderr.include?("resource limit exceeded")
    PluginResult.new("", "resource_limit", result.exit_code(), stderr.strip())
  else
    PluginResult.new("", "crashed", result.exit_code(), stderr.strip())
  end
end

def run_plugin(diamond_bin, spec)
  argv = build_argv(diamond_bin, spec)
  result = Process.run(argv)
  outcome = classify(result)
  PluginResult.new(spec["name"], outcome.status(), outcome.exit_code(), outcome.note())
end

def report_line(outcome)
  case outcome.status()
  when "ok" then "  ok             #{outcome.note()}"
  when "sandbox_denied" then "  sandbox_denied #{outcome.note().split("\n")[0]}"
  when "resource_limit" then "  resource_limit #{outcome.note().split("\n")[0]}"
  else "  crashed        exit #{outcome.exit_code()}: #{outcome.note().split("\n")[0]}"
  end
end

def main(argv)
  if argv.empty?()
    puts("usage: host.di <path-to-diamond-binary>")
    return 64
  end
  diamond_bin = argv[0]
  manifest = JSON.parse(File.open("manifest.json", "r").read())
  exit_status = 0
  manifest["plugins"].each() do |spec|
    outcome = run_plugin(diamond_bin, spec)
    puts(outcome.name())
    puts(report_line(outcome))
    if outcome.status() == "crashed"
      exit_status = 1
    end
  end
  exit_status
end

exit(main(ARGV))
