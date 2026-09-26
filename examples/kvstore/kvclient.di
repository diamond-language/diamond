# kvclient: send requests to a kvserver and print each reply.
#
#   diamond kvclient.di PORT 'SET greeting hello' 'GET greeting'
#   echo 'KEYS' | diamond kvclient.di PORT      (requests from stdin)
#
# Uses a plain blocking TCPSocket. Exits 1 if any reply was an error.
def main(args: Array[String]) -> Int
  if args.empty?() || args[0].to_i() < 1
    warn("usage: kvclient PORT [REQUEST...]")
    return 64
  end
  requests = args.drop(1)
  if requests.empty?()
    loop do
      line = gets()
      break if line == nil
      requests.push(line) unless line.strip().empty?()
    end
  end
  socket = nil
  begin
    socket = TCPSocket.connect("127.0.0.1", args[0].to_i(), {"connect_timeout_ms": 2000, "read_timeout_ms": 5000})
  rescue error: IOError
    warn("kvclient: #{error.message()}")
    return 69
  end
  failed = false
  # A while loop so a closed connection can stop it: `break` inside a
  # do-block isn't supported.
  index = 0
  while index < requests.length()
    socket.write("#{requests[index]}\n")
    reply = socket.gets()
    break if reply == nil
    reply = reply.rstrip()
    puts(reply)
    failed = true if reply.start_with?("-")
    index += 1
  end
  socket.close()
  if failed then 1 else 0 end
end

exit(main(ARGV))
