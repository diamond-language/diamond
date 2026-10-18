# A sandbox that allows subprocess but not filesystem must refuse Process.spawn output files.
probe = "/tmp/diamond_sandbox_spawn_probe.log"
denied = false
begin
  Process.spawn(["true"], {"stdout": probe})
rescue error: SandboxError
  denied = true
end
piped = Process.spawn(["true"], {"stderr_to_stdout": true})
piped.wait()
created = Process.run(["test", "-e", probe]).exit_code() == 0
[denied, created]
