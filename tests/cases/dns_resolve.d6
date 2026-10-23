puts(DNS.resolve("127.0.0.1", [], nil) == ["127.0.0.1"])
puts(DNS.resolve("::1", [], nil) == ["::1"])
puts(DNS.resolve("localhost", [], Time.monotonic() + 5.0).length() > 0)
cancel = Channel.new(1)
cancel.close()
puts(DNS.resolve("never-start.test", [cancel, cancel], nil) == nil)
puts(DNS.resolve("never-start.test", [], Time.monotonic() - 1.0) == nil)
errors = 0
[[nil, [], nil], ["", [], nil], ["[::1]", [], nil], ["::1%lo0", [], nil], ["bad" + chr(0), [], nil], ["localhost", nil, nil], ["localhost", [1], nil], ["localhost", [], 1]].each() do |args|
  begin
    DNS.resolve(args[0], args[1], args[2])
    raise "invalid resolver options accepted"
  rescue error: TypeError
    errors += 1
  end
end
puts(errors)
