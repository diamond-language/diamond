# Drives shutdown.di: token-driven HTTP draining with real silent and slow clients.
#
#   diamond shutdown_driver.di PROGRAM...
require "../../../tests/harness"

command = ARGV

def run_mode(command, mode)
  Harness.with_port(5) do |port|
    child = Harness::Child.new(command + [port.to_s(), mode], {})
    peer = nil
    begin
      if mode != "pre" && mode != "deadline"
        Harness.wait_for(5000, "HTTP startup") do
          raise "the server exited during startup: #{child.stdout_text()}#{child.stderr_text()}" unless child.running?()
          begin
            Harness.http_get(port, "/health") == [200, "ok"]
          rescue error: IOError
            false
          end
        end
        if mode != "idle"
          options = {"connect_timeout_ms": 3000, "read_timeout_ms": 3000}
          options["recv_buffer_bytes"] = 4096 if mode == "slow"
          peer = TCPSocket.connect("127.0.0.1", port, options)
          if mode == "slow" || mode == "drain" || mode == "zero"
            path = mode == "slow" ? "/large" : "/hold"
            peer.write("GET #{path} HTTP/1.1\r\nHost: localhost\r\n\r\n")
            peer.flush()
            child.wait_for_text(mode == "slow" ? "handler writing" : "handler waiting", 5000)
          else
            raise "health check failed" unless Harness.http_get(port, "/health")[0] == 200
          end
        end
        raise "trigger failed" unless Harness.http_get(port, "/trigger") == [200, "triggered"]
        child.wait_for_text("\"message\":\"server.shutdown_started\"", 5000)
        # The listener must close while accepted clients are still draining.
        stayed_open = true
        begin
          extra = TCPSocket.connect("127.0.0.1", port, {"connect_timeout_ms": 200})
          extra.close()
        rescue error: IOError
          stayed_open = false
        end
        raise "listener remained open during shutdown" if stayed_open
        if mode == "drain"
          peer.write("x")
          peer.flush()
          received = peer.read()
          raise "unexpected drain response #{received.inspect()}" unless received.end_with?("\r\n\r\nok")
        end
      end
      code = child.wait_exit(5000)
      raise "the server did not exit" if code == nil
      raise "exit code #{code}" unless code == 0
      output = child.stdout_text() + child.stderr_text()
      raise "owner did not clean up before returning" unless child.stdout_text().include?("owner cleaned\nreturned")
      forced = output.include?("\"forced\":true")
      expect_forced = mode == "silent" || mode == "slow" || mode == "zero"
      raise "forced shutdown was #{forced}, expected #{expect_forced}" unless forced == expect_forced
      if mode == "slow" || mode == "drain" || mode == "zero"
        raise "handler did not clean up" unless output.include?("handler cleaned")
      end
      if mode == "silent" || mode == "zero"
        raise "silent socket leaked" unless peer.read(1) == ""
      end
      puts("passed #{mode}")
    rescue error: StandardError
      text = child.stdout_text() + child.stderr_text()
      if text.include?("Address already in use")
        raise Harness::PortTaken.new("Gremlin shutdown scenario #{mode}:\n#{text}")
      end
      puts("Gremlin shutdown scenario #{mode}:\n#{text}")
      raise error
    ensure
      peer.close() if peer != nil
      child.finish()
    end
  end
end

["idle", "silent", "slow", "drain", "zero", "pre", "deadline"].each() do |mode|
  run_mode(command, mode)
end
puts("gremlin token shutdown tests passed")
exit(0)
