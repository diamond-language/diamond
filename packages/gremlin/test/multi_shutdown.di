require "../lib/gremlin"
require "../../cancellation/lib/cancellation"

class TestOwner
  def self.mark()
    @@owner = true
  end
  def self.owner?() = @@owner == true
end

def handle(request, context)
  if context["id"] == nil then context["id"] = (if TestOwner.owner?() then "owner " else "child " end) + Time.monotonic().to_s() end
  if request["path"] == "/hold" || request["path"] == "/large"
    conn = context["gremlin_connection"]
    begin
      conn.write(context["id"] + "\n")
      if request["path"] == "/large"
        conn.write("x".repeat(8388608))
      else
        if conn.read(1) == "!" then raise "worker failed" end
        conn.write("done\n")
      end
    ensure
      puts("cleaned " + GremlinShutdown.deadline().to_s())
    end
    return nil
  end
  [200, {}, "ok"]
end

def tick(context)
  unless context["ready"] == true
    context["ready"] = true
    puts("worker ready")
  end
end

def run(port, mode)
  TestOwner.mark()
  source = Cancellation::Source.new(nil, if mode == "deadline" then 0.3 else nil end)
  def cancel() = source.cancel()
  Signal.trap("TERM", cancel)
  if mode == "pre" then source.cancel() end
  begin
    gremlin_serve(port, handle, 3, 0.01, tick, nil, false, source.token(), 0.4)
  rescue error
    if mode == "bind" && error is IOError
      puts("bind failed")
    elsif mode == "failure" && error == "worker failed"
      puts("worker failure propagated")
    else
      raise error
    end
  ensure
    puts("owner cleaned")
  end
  puts("returned")
end
run(ARGV[0].to_i(), ARGV[1])
exit(0)
