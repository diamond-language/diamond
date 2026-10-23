# Process.run waiting on a child is interrupted, and the child is killed.
def run_sleep() = Process.run(["sleep", "60"])

sup = Supervisor.new()
sup.add_child(run_sleep)
started = Time.monotonic()
puts(sup.stop(100))
puts(sup.alive?(0))
puts(Time.monotonic() - started < 10)
