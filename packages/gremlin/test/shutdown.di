require "../lib/gremlin"
require "../../cancellation/lib/cancellation"

class ShutdownTest
  def self.boot(gate)
    @@gate = gate
  end
  def self.handle(request, context)
    if request["path"] == "/trigger"
      @@gate.send(true)
      return [200, {}, "triggered"]
    end
    if request["path"] == "/hold" || request["path"] == "/large"
      conn = context["gremlin_connection"]
      begin
        if request["path"] == "/hold"
          puts("handler waiting")
          conn.read(1)
          conn.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
        else
          puts("handler writing")
          conn.write("HTTP/1.1 200 OK\r\nContent-Length: 8388608\r\n\r\n" + "x".repeat(8388608))
        end
      ensure
        puts("handler cleaned")
      end
      return nil
    end
    [200, {}, "ok"]
  end
end

def cancel_after_request(source, gate)
  gate.receive()
  source.cancel()
end

def run(port, mode)
  source = Cancellation::Source.new(nil, if mode == "deadline" then 0.2 else nil end)
  gate = Channel.new(1)
  ShutdownTest.boot(gate)
  worker = nil
  if mode == "pre"
    [-1, "bad", nil].each() do |timeout|
      begin
        gremlin_serve(port, ShutdownTest.handle, 1, nil, nil, nil, false, nil, timeout)
        raise "invalid shutdown timeout accepted"
      rescue error: ArgumentError
        nil
      end
    end
    source.cancel()
  elsif mode != "deadline"
    worker = Thread.new(cancel_after_request, source, gate)
  end
  begin
    gremlin_serve(port, ShutdownTest.handle, 1, nil, nil, nil, false,
      source.token(), if mode == "zero" then 0.0 elsif mode == "drain" then 1.0 else 0.3 end)
  ensure
    if worker != nil then worker.join() end
    puts("owner cleaned")
  end
  puts("returned")
end
run(ARGV[0].to_i(), ARGV[1])
exit(0)
