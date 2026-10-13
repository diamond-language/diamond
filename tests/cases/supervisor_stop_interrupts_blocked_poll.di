# IO.poll with no timeout, and a sleep built on it, are interrupted.
def forever()
  IO.poll([], [], -1)
end

def long_sleep()
  IO.poll([], [], 60000)
end

sup = Supervisor.new()
sup.add_child(forever)
sup.add_child(long_sleep)
puts(sup.stop(100))
puts(sup.alive?(0))
puts(sup.alive?(1))
puts(sup.last_error(1))
