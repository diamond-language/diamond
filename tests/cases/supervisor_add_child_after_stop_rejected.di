def noop()
  1
end
sup = Supervisor.new()
sup.stop()
sup.add_child(noop)
