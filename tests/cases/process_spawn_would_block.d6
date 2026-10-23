h = Process.spawn(["sleep", "1"])

begin
  h.stdout().read(10)
  puts("no raise")
rescue error: WouldBlockError
  puts("would block")
end

h.wait()

s = h.stdout()
s.close()
begin
  s.read(10)
  puts("no raise")
rescue error: IOError
  puts("stream closed")
end
