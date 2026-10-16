# Thread-count sweep for gremlin_serve under a small, mixed-work endpoint.
#
# For each worker-thread count the script starts a fresh server, warms it up, then
# measures steady-state throughput. Rounds are interleaved (every thread count once per
# round, in rotating order) so slow drift on the host (thermal state, other processes)
# spreads across all configurations instead of favouring whichever ran first. Results
# are averaged over the rounds.
#
# The load generator is several concurrent Apache Bench processes: one `ab` is
# single-threaded and would cap out before the server does. gremlin has no keep-alive,
# so `ab`'s default of one connection per request matches how the server is actually
# used. The generator shares the machine with the server, so at high thread counts they
# compete for cores; that is part of what this measures. Pin them apart with
# --server-cpus and --ab-cpus (needs `taskset`) to stop that.
#
# --instrument also samples what the machine is doing during each measured window: CPU
# frequency of the cores the server may use, the user/system split of the server process,
# context switches per request, and how evenly the worker threads share the work (Linux
# only: it reads /proc and /sys). Used by docs/internal/gremlin-scaling-investigation.md.
#
# Usage: diamond bench/gremlin_http/sweep.di [--threads 2,4,6,8,10,12] [--rounds 5]
#          [--warmup 5] [--measure 10] [--clients 3] [--concurrency 32]
#          [--server-cpus 0-9] [--ab-cpus 10,11] [--workload mixed|hello|cpu]
#          [--instrument]
# Environment: DIAMOND_BIN (default: build/diamond, a `make release` build).
# Needs `ab` (apache2-utils / httpd-tools) on PATH.

require "../../tests/harness"

# "0-3,8" => [0, 1, 2, 3, 8]
def parse_cpus(spec: String)
  cpus = []
  spec.split(",").each() do |part|
    if part.include?("-")
      bounds = part.split("-")
      first = bounds[0].to_i()
      last = bounds[1].to_i()
      (first..last).each() do |cpu|
        cpus.push(cpu)
      end
    else
      cpus.push(part.to_i())
    end
  end
  cpus
end

# Current frequency in MHz of each of `cpus` that exposes one.
def read_freqs(cpus: Array)
  freqs = []
  cpus.each() do |cpu|
    path = "/sys/devices/system/cpu/cpu#{cpu}/cpufreq/scaling_cur_freq"
    freqs.push(File.read(path).strip().to_i() / 1000.0) if File.exist?(path)
  end
  freqs
end

# The number in a /proc status line such as "VmRSS:\t  1234 kB".
def status_number(line: String)
  line[line.index(":") + 1..-1].strip().split(" ")[0].to_i()
end

# CPU ticks (user, system and per thread) and the summed voluntary / involuntary context
# switches of a process.
def task_stats(pid: Int)
  user = 0
  system = 0
  voluntary = 0
  involuntary = 0
  threads = {}
  Dir.entries("/proc/#{pid}/task").each() do |tid|
    begin
      stat = File.read("/proc/#{pid}/task/#{tid}/stat")
      fields = stat[stat.rindex(")") + 1..-1].strip().split(" ")
      utime = fields[11].to_i()
      stime = fields[12].to_i()
      user += utime
      system += stime
      threads[tid] = utime + stime
      File.read("/proc/#{pid}/task/#{tid}/status").split("\n").each() do |line|
        if line.start_with?("voluntary_ctxt_switches")
          voluntary += status_number(line)
        elsif line.start_with?("nonvoluntary_ctxt_switches")
          involuntary += status_number(line)
        end
      end
    rescue error: IOError
      # The thread exited between the listing and the read.
    end
  end
  {"user": user, "system": system, "threads": threads, "voluntary": voluntary, "involuntary": involuntary}
end

def rss_mb(pid: Int)
  File.read("/proc/#{pid}/status").split("\n").each() do |line|
    return status_number(line) / 1024.0 if line.start_with?("VmRSS")
  end
  0.0
end

def mean(values: Array)
  return 0.0 if values.length() == 0
  total = 0.0
  values.each() do |v|
    total += v
  end
  total / values.length()
end

def stdev(values: Array)
  return 0.0 if values.length() < 2
  m = mean(values)
  squares = 0.0
  values.each() do |v|
    squares += (v - m) * (v - m)
  end
  (squares / (values.length() - 1)).sqrt()
end

def pad(value, width: Int)
  value.to_s().rjust(width, " ")
end

class Sweep
  def initialize(diamond: String, root: String, project: String, options: Hash)
    @diamond = diamond
    @root = root
    @project = project
    @options = options
    @freqs = []
    @threads = 0
  end

  # The first word after `label` on the first line of `text` that starts with it, or nil.
  def number_after(text: String, label: String)
    text.split("\n").each() do |line|
      stripped = line.strip()
      next unless stripped.start_with?(label)
      return stripped[label.length()..-1].strip().split(" ")[0]
    end
    nil
  end

  # Everything the finished process wrote to `stream`.
  def drain(stream)
    text = ""
    loop do
      chunk = nil
      begin
        chunk = stream.read(65536)
      rescue error: WouldBlockError
        break
      end
      break if chunk == nil || chunk == ""
      text += chunk
    end
    text
  end

  # Starts `clients` ab processes for `seconds` and waits for them all. With a `pid`,
  # also samples the CPU frequency once a second while they run. Returns
  # [requests/sec summed, failed requests summed, worst p99 in ms].
  def load(port: Int, seconds: Int, pid, cpus: Array)
    url = "http://127.0.0.1:#{port}/bench"
    prefix = @options["ab_cpus"] == "" ? [] : ["taskset", "-c", @options["ab_cpus"]]
    handles = []
    @options["clients"].times() do |i|
      handles.push(Process.spawn(prefix + ["ab", "-q", "-t", seconds.to_s(), "-n", "100000000",
                                           "-c", @options["concurrency"].to_s(), url]))
    end
    if pid != nil
      @freqs = []
      while handles[0].running?()
        Harness.sleep_ms(1000)
        top = read_freqs(cpus).sort().reverse()
        wanted = top.length() < @threads ? top.length() : @threads
        @freqs.push(mean(top[0...wanted])) if wanted > 0
      end
    end
    rps = 0.0
    failed = 0
    p99 = 0.0
    handles.each() do |handle|
      handle.wait()
      text = self.drain(handle.stdout())
      value = self.number_after(text, "Requests per second:")
      raise "ab produced no result:\n#{text}" if value == nil
      rps += value.to_f()
      failures = self.number_after(text, "Failed requests:")
      failed += failures.to_i() unless failures == nil
      tail = self.number_after(text, "99%")
      p99 = tail.to_f() if tail != nil && tail.to_f() > p99
    end
    [rps, failed, p99]
  end

  # One server with `threads` workers, warmed up and then measured. Returns a Hash of
  # "rps", "p99", "failed" and, with --instrument, the machine-level numbers too.
  def trial(threads: Int, port: Int)
    @threads = threads
    instrument = @options["instrument"]
    env = {"SWEEP_PORT": port.to_s(), "SWEEP_THREADS": threads.to_s(),
           "SWEEP_WORKLOAD": @options["workload"]}
    argv = [@diamond, "#{@root}/bench/gremlin_http/server.di"]
    argv = ["taskset", "-c", @options["server_cpus"]] + argv unless @options["server_cpus"] == ""
    server = Process.spawn(argv, {"cwd": @project, "env": env, "stdout": "/dev/null",
                                  "stderr_to_stdout": true})
    begin
      Harness.wait_for(10000, "server with #{threads} threads failed to start") do
        up = true
        begin
          TCPSocket.connect("127.0.0.1", port).close()
        rescue error: IOError
          up = false
        end
        up
      end
      cpu_spec = @options["server_cpus"] == "" ? "0-#{@options["cpu_count"] - 1}" : @options["server_cpus"]
      cpus = parse_cpus(cpu_spec)
      pid = instrument ? server.pid() : nil
      self.load(port, @options["warmup"], nil, cpus)
      before = instrument ? task_stats(pid) : nil
      rss_before = instrument ? rss_mb(pid) : 0.0
      started = Time.monotonic()
      measured = self.load(port, @options["measure"], pid, cpus)
      result = {"rps": measured[0], "failed": measured[1], "p99": measured[2]}
      return result unless instrument

      elapsed = Time.monotonic() - started
      after = task_stats(pid)
      ticks = @options["clock_ticks"]
      user = (after["user"] - before["user"]) / ticks
      system = (after["system"] - before["system"]) / ticks
      requests = measured[0] * elapsed
      busy = []
      after["threads"].each() do |tid, total|
        used = (total - (before["threads"][tid] || 0)) / ticks
        busy.push(used) if used > 0.05 * elapsed
      end
      busy = busy.sort()
      result["user_cpu_pct"] = 100 * user / elapsed
      result["sys_cpu_pct"] = 100 * system / elapsed
      result["cpu_ms_per_req"] = 1000 * (user + system) / requests
      result["ctx_vol_per_req"] = (after["voluntary"] - before["voluntary"]) / requests
      result["ctx_invol_per_req"] = (after["involuntary"] - before["involuntary"]) / requests
      result["mean_mhz"] = mean(@freqs)
      result["busy_threads"] = busy.length()
      result["thread_cpu_spread"] = busy.length() == 0 ? 0.0 : busy[0] / busy[busy.length() - 1]
      rss_after = rss_mb(pid)
      result["rss_mb"] = rss_after
      result["rss_growth_mb"] = rss_after - rss_before
      result
    ensure
      server.terminate()
      server.wait()
    end
  end
end

def option(args: Array, name: String, fallback: String)
  index = args.index(name)
  return fallback if index == nil
  args[index + 1]
end

def column(rows: Array, key: String)
  rows.map() do |row|
    row[key]
  end
end

def print_sweep(counts: Array, rows: Hash)
  puts("\nthreads |  mean req/s |  stdev | stdev% |    min |    max | mean p99 ms | failed")
  puts("--------|-------------|--------|--------|--------|--------|-------------|-------")
  best = counts[0]
  counts.each() do |n|
    best = n if mean(column(rows[n.to_s()], "rps")) > mean(column(rows[best.to_s()], "rps"))
  end
  counts.each() do |threads|
    values = column(rows[threads.to_s()], "rps")
    m = mean(values)
    sd = stdev(values)
    failed = 0
    column(rows[threads.to_s()], "failed").each() do |n|
      failed += n
    end
    marker = threads == best ? "  <- peak" : ""
    puts("#{pad(threads, 7)} | #{pad(m.round(), 11)} | #{pad(sd.round(), 6)} | " +
         "#{pad((100 * sd / m).round(1), 5)}% | #{pad(values.min().round(), 6)} | " +
         "#{pad(values.max().round(), 6)} | #{pad(mean(column(rows[threads.to_s()], "p99")).round(), 11)} | " +
         "#{pad(failed, 6)}#{marker}")
  end
end

def print_instrumented(counts: Array, rows: Hash)
  keys = ["rps", "p99", "cpu_ms_per_req", "user_cpu_pct", "sys_cpu_pct", "mean_mhz",
          "ctx_vol_per_req", "ctx_invol_per_req", "busy_threads", "thread_cpu_spread",
          "rss_mb", "rss_growth_mb", "failed"]
  titles = keys.map() do |k|
    pad(k, 17)
  end
  puts("threads | " + titles.join(" | "))
  counts.each() do |threads|
    cells = keys.map() do |k|
      pad(mean(column(rows[threads.to_s()], k)).round(2), 17)
    end
    puts("#{pad(threads, 7)} | " + cells.join(" | "))
  end
end

def run(args: Array)
  root = File.dirname(File.dirname(__dir__))
  diamond = ENV["DIAMOND_BIN"] || "#{root}/build/diamond"
  instrument = args.include?("--instrument")
  options = {
    "rounds": option(args, "--rounds", instrument ? "2" : "5").to_i(),
    "warmup": option(args, "--warmup", instrument ? "4" : "5").to_i(),
    "measure": option(args, "--measure", instrument ? "8" : "10").to_i(),
    "clients": option(args, "--clients", "3").to_i(),
    "concurrency": option(args, "--concurrency", "32").to_i(),
    "server_cpus": option(args, "--server-cpus", ""),
    "ab_cpus": option(args, "--ab-cpus", ""),
    "workload": option(args, "--workload", "mixed"),
    "instrument": instrument,
    "cpu_count": Process.run(["nproc"]).stdout().strip().to_i(),
    "clock_ticks": Process.run(["getconf", "CLK_TCK"]).stdout().strip().to_i(),
  }
  unless ["mixed", "hello", "cpu"].include?(options["workload"])
    warn("--workload must be mixed, hello or cpu")
    exit(2)
  end
  counts = option(args, "--threads", instrument ? "2,6,12" : "2,4,6,8,10,12").split(",").map() do |n|
    n.to_i()
  end
  rows = {}
  counts.each() do |n|
    rows[n.to_s()] = []
  end

  version = Process.run([diamond, "--version"]).stdout().strip()
  puts("#{version}; #{options["cpu_count"]} logical CPUs; workload=#{options["workload"]}; ab x#{options["clients"]} at " +
       "-c #{options["concurrency"]} (#{options["clients"] * options["concurrency"]} connections total)")
  puts("server cpus: #{options["server_cpus"] == "" ? "any" : options["server_cpus"]}; ab cpus: " +
       "#{options["ab_cpus"] == "" ? "any" : options["ab_cpus"]}")
  puts("#{options["rounds"]} rounds x #{counts.length()} thread counts, #{options["warmup"]}s warmup + " +
       "#{options["measure"]}s measured each")

  # A throwaway project whose cuts/ links the in-repo gremlin, http and logger.
  project = Harness.make_temp_dir()
  Dir.mkdir("#{project}/cuts", 0o755)
  ["gremlin", "http", "logger"].each() do |name|
    Process.run(["ln", "-s", "#{root}/packages/#{name}", "#{project}/cuts/#{name}"])
  end
  sweep = Sweep.new(diamond, root, project, options)

  options["rounds"].times() do |round|
    # Rotate the starting point so no thread count is always first or last.
    shift = round % counts.length()
    order = counts[shift..-1] + counts[0...shift]
    order.each() do |threads|
      result = sweep.trial(threads, Harness.reserve_port())
      rows[threads.to_s()].push(result)
      puts("round #{round + 1} threads=#{pad(threads, 2)}: #{pad(result["rps"].round(), 9)} req/s " +
           "(p99 #{result["p99"].round()} ms, #{result["failed"]} failed)")
    end
  end
  Harness.remove_tree(project)

  if instrument
    puts("")
    print_instrumented(counts, rows)
  else
    print_sweep(counts, rows)
  end
end

run(ARGV)
exit(0)
