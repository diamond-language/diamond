# A read from a named pipe whose writer stays silent is interrupted by
# Supervisor#stop(timeout_ms). The writer is a background `sleep` holding the
# pipe open.
FIFO = "/tmp/diamond_stop_fifo_test"
Process.run(["sh", "-c", "rm -f " + FIFO + "; mkfifo " + FIFO + " && { (sleep 4 > " + FIFO + " &) > /dev/null 2>&1 < /dev/null; }; exit 0"])

def reader()
  f = File.open(FIFO, "r")
  f.read(1)
end

sup = Supervisor.new()
sup.add_child(reader)
started = Time.monotonic()
puts(sup.stop(300))
puts(Time.monotonic() - started < 2.0)
puts(sup.alive?(0))
Process.run(["rm", "-f", FIFO])
nil
