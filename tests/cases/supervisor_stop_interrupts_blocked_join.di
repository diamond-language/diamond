# A supervised worker joining a thread that never returns is interrupted.
def hang() = Channel.new(1).receive()

def joiner() = Thread.new(hang).join()

sup = Supervisor.new()
sup.add_child(joiner)
puts(sup.stop(100))
puts(sup.alive?(0))
puts(sup.last_error(0))
