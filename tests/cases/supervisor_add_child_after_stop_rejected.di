def noop() = 1
sup = Supervisor.new()
sup.stop()
sup.add_child(noop)
