def worker()
  begin
    File.open("/tmp/diamond_sandbox_allow_thread_probe.txt", "w")
    "fs escaped"
  rescue error: SandboxError
    "fs denied"
  end
end
t = Thread.new(worker)
t.join()
