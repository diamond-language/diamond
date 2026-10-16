# Drives dns_test.di with deterministic resolver stalls: a preloaded shim holds `getaddrinfo`
# until a release file appears, while the production worker and wait paths stay real.
#
#   diamond dns_driver.di PROGRAM...
require "../../../tests/harness"

command = ARGV

def count_started(marker)
  return 0 unless File.exist?(marker)
  File.read(marker).split("started").length() - 1
end

def preload_for(shim)
  parts = []
  prefix = ENV["DNS_TEST_PRELOAD_PREFIX"]
  parts.push(prefix) if prefix != nil && prefix != ""
  parts.push(shim)
  parts.join(":")
end

work = Process.run(["mktemp", "-d"]).stdout().strip()
begin
  compiler = ENV["CC"]
  compiler = "cc" if compiler == nil || compiler == ""
  shim = "#{work}/resolver.so"
  built = Process.run([compiler, "-shared", "-fPIC", "-o", shim, "test/resolver_shim.c", "-ldl"])
  raise "cannot build the resolver shim: #{built.stderr()}" unless built.success?()

  ["success", "missing", "deadline", "cancel", "signal", "signal_error", "capacity", "parallel",
   "budget"].each() do |mode|
    marker = "#{work}/#{mode}.started"
    release = "#{work}/#{mode}.release"
    listener = TCPServer.listen(0, host: "127.0.0.1", backlog: 1)
    port = listener.port()
    fillers = []
    if mode == "budget"
      saturated = false
      128.times() do |attempt|
        break if saturated
        begin
          fillers.push(TCPSocket.connect("127.0.0.1", port, {"connect_timeout_ms": 100}))
        rescue error: IOError
          saturated = true
        end
      end
      raise "could not fill the local listener backlog" unless saturated
    end
    env = {"LD_PRELOAD": preload_for(shim), "DNS_TEST_STARTED": marker,
           "DNS_TEST_RELEASE": release, "DIAMOND_NO_CACHE": "1"}
    child = Harness::Child.new(command + [mode, port.to_s(), marker], env)
    begin
      if mode == "success"
        peer = listener.accept()
        raise "client did not send hello" unless peer.read(5) == "hello"
        raise "client did not close" unless peer.read(1) == ""
        peer.close()
        child.line("success", 10000)
      elsif mode == "missing"
        child.line("missing", 10000)
      elsif mode == "parallel"
        child.line("parallel bounded", 10000)
        raise "expected 8 resolver threads" unless count_started(marker) == 8
      elsif mode == "capacity"
        child.line("baseline", 10000)
        baseline = Harness.descriptor_count(child.pid())
        child.send("go\n")
        child.line("saturated", 10000)
        raise "expected 8 resolver threads" unless count_started(marker) == 8
        if baseline != nil && Harness.descriptor_count(child.pid()) > baseline + 16
          raise "unbounded resolver descriptors"
        end
        File.write(release, "")
        child.send("go\n")
        child.line("recovered", 10000)
        if baseline != nil && Harness.descriptor_count(child.pid()) != baseline
          raise "abandoned resolver leaked descriptors"
        end
        child.send("go\n")
      else
        if mode == "signal" || mode == "signal_error"
          Harness.wait_for(10000, "the lookup never started") do
            File.exist?(marker) && child.running?()
          end
          child.terminate()
        end
        expected = mode == "signal_error" ? "signal error" :
          (mode == "deadline" || mode == "budget") ? "deadline" : "cancelled"
        child.line(expected, 10000)
      end
      child.line("cleaned", 10000)
      # No release file in the cancellation cases: libc is still stuck when the VM exits, so
      # joining a resolver here would hang forever.
      raise "#{mode}: unclean exit" unless child.wait() == 0
      if (mode == "cancel" || mode == "signal") && !File.exist?(marker)
        raise "cancelled before the lookup actually started"
      end
    rescue error: StandardError
      puts("DNS scenario #{mode}: #{child.errors()}")
      raise error
    ensure
      File.write(release, "")
      child.finish()
      fillers.each() do |filler|
        filler.close()
      end
      listener.close()
    end
  end
ensure
  Process.run(["rm", "-rf", work])
end
puts("cancellation DNS tests passed")
