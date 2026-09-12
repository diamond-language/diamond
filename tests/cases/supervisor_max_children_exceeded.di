def noop()
  1
end
sup = Supervisor.new()
count = 0
while count < 32
  sup.add_child(noop)
  count = count + 1
end
sup.add_child(noop)
