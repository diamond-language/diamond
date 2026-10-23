# stop(timeout_ms) lets attempts finish for up to the timeout, then
# interrupts the rest; it answers whether it had to.
def spin()
  count = 0
  while true
    count += 1
  end
end

def quick() = 1 + 1

sup = Supervisor.new()
sup.add_child(spin)
sup.add_child(quick)
puts(sup.stop(100))
puts(sup.alive?(0))
puts(sup.alive?(1))
puts(sup.last_error(0))
puts(sup.restart_count(0))
puts(sup.last_error(1) == nil)

calm = Supervisor.new()
calm.add_child(quick)
puts(calm.stop(2000))
