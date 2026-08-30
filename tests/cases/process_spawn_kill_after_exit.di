h = Process.spawn(["true"])
h.wait()

begin
  h.terminate()
  "no raise"
rescue error: IOError
  puts("terminate guarded")
end

begin
  h.kill()
  "no raise"
rescue error: IOError
  puts("kill guarded")
end
