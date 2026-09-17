def worker()
  begin
    File.open("/tmp/diamond_sandbox_thread_probe.txt", "w")
    "escaped"
  rescue error: SandboxError
    error.message()
  end
end
t = Thread.new(worker)
t.join()
