# Drives multi_shutdown.di: cancellation, a common deadline and joins across isolated HTTP VMs.
#
#   diamond multi_shutdown_driver.di PROGRAM...
require "../../../tests/harness"

command = ARGV

def count_of(text, needle)
  text.split(needle).length() - 1
end

def run_mode(command, mode)
  bound = nil
  begin
    Harness.with_port(5) do |port|
      # The `bind` scenario holds the port on purpose and expects the server to report failure.
      bound = TCPServer.listen(port, host: "127.0.0.1") if mode == "bind"
      child = Harness::Child.new(command + [port.to_s(), mode], {})
      peers = []
      begin
        if mode != "pre" && mode != "deadline" && mode != "bind"
          Harness.wait_for(8000, "workers never became ready") do
            child.pump(10)
            raise "the server exited: #{child.stdout_text()}#{child.stderr_text()}" unless child.running?()
            count_of(child.stdout_text() + child.stderr_text(), "worker ready") == 3
          end
          if mode == "forced" || mode == "slow" || mode == "drain" || mode == "failure"
            # Keep one parked request on each worker. Reuse-port routing is nondeterministic,
            # so workers are identified from their responses.
            workers = []
            child_peer = nil
            120.times() do |attempt|
              break if workers.length() == 3
              options = {"connect_timeout_ms": 2000, "read_timeout_ms": 2000}
              options["recv_buffer_bytes"] = 4096 if mode == "slow"
              peer = TCPSocket.connect("127.0.0.1", port, options)
              path = mode == "slow" ? "/large" : "/hold"
              peer.write("GET #{path} HTTP/1.1\r\nHost: localhost\r\n\r\n")
              peer.flush()
              identity = peer.gets()
              raise "a worker sent no identity" if identity == nil
              if workers.include?(identity)
                if mode != "slow"
                  peer.write("x")
                  peer.flush()
                  peer.read()
                end
                peer.close()
              else
                workers.push(identity)
                peers.push(peer)
                child_peer = peer if identity.start_with?("child ")
              end
            end
            raise "did not reach all workers" unless workers.length() == 3
          end
          if mode == "failure"
            raise "no child worker" if child_peer == nil
            child_peer.write("!")
            child_peer.flush()
          else
            child.terminate()
          end
          started = mode == "failure" ? 2 : 3
          Harness.wait_for(8000, "shutdown never started") do
            count_of(child.stdout_text() + child.stderr_text(), "\"message\":\"server.shutdown_started\"") == started
          end
          stayed_open = true
          begin
            extra = TCPSocket.connect("127.0.0.1", port, {"connect_timeout_ms": 200})
            extra.close()
          rescue error: IOError
            stayed_open = false
          end
          raise "a worker listener remained open" if stayed_open
          if mode == "drain"
            peers.each() do |peer|
              peer.write("x")
              peer.flush()
              response = peer.read()
              raise "unexpected response #{response.inspect()}" unless response == "done\n"
            end
          end
        end
        code = child.wait_exit(8000)
        raise "the server did not exit" if code == nil
        raise "exit code #{code}" unless code == 0
        out = child.stdout_text()
        output = out + child.stderr_text()
        raise "owner did not clean up before returning" unless out.include?("owner cleaned\nreturned")
        raise "worker failure was not propagated" if mode == "failure" && !output.include?("worker failure propagated")
        if mode == "bind"
          raise "bind failure not reported" unless output.include?("bind failed")
        else
          done = mode == "failure" ? 2 : 3
          raise "wrong shutdown_complete count" unless count_of(output, "\"message\":\"server.shutdown_complete\"") == done
          forced = mode == "failure" ? 2 : (mode == "forced" || mode == "slow") ? 3 : 0
          raise "wrong forced count" unless count_of(output, "\"forced\":true") == forced
        end
        if mode == "forced" || mode == "slow" || mode == "drain" || mode == "failure"
          deadlines = out.scan(Regexp.new("cleaned [0-9]+(?:\\.[0-9]+)?"))
          expected = mode == "failure" ? 2 : 3
          raise "wrong number of cleanup deadlines" unless deadlines.length() == expected
          raise "workers disagree on the shutdown deadline" unless deadlines.uniq().length() == 1
          if mode != "slow"
            peers.each() do |peer|
              raise "socket leaked after owner returned" unless peer.read(1) == ""
            end
          end
        end
        puts("passed #{mode}")
      rescue error: StandardError
        text = child.stdout_text() + child.stderr_text()
        if mode != "bind" && text.include?("Address already in use")
          raise Harness::PortTaken.new("Multi-worker shutdown #{mode}:\n#{text}")
        end
        puts("Multi-worker shutdown #{mode}:\n#{text}")
        raise error
      ensure
        peers.each() do |peer|
          peer.close()
        end
        child.finish()
      end
    end
  ensure
    bound.close() if bound != nil
  end
end

["idle", "forced", "slow", "drain", "failure", "pre", "deadline", "bind"].each() do |mode|
  run_mode(command, mode)
end
puts("gremlin multi-worker shutdown tests passed")
exit(0)
