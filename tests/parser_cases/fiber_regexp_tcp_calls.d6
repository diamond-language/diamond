def make()
 def once()
  1
 end
 once
end
f = Fiber.new(make())
puts(f)
re = Regexp.new("FOO", 3)
puts(re.match?("foo"))
server = TCPServer.listen(0)
server.close()
puts("done")
