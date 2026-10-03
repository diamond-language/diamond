# :rest_for_one restarts the crashed child and the children added after
# it, but not the ones added before.
def idle(starts, quit)
  starts.send(1)
  loop
    break if quit.closed?()
  end
end

def crasher(a_starts, starts, c_starts, quit)
  starts.send(1)
  if starts.size() == 1
    loop
      break if a_starts.size() >= 1 && c_starts.size() >= 1
    end
    raise "boom"
  end
  loop
    break if quit.closed?()
  end
end

a_starts = Channel.new(10)
b_starts = Channel.new(10)
c_starts = Channel.new(10)
quit = Channel.new(1)
sup = Supervisor.new(:rest_for_one)
sup.add_child(idle, a_starts, quit)
sup.add_child(crasher, a_starts, b_starts, c_starts, quit)
sup.add_child(idle, c_starts, quit)
loop
  break if c_starts.size() >= 2
end
quit.close()
sup.stop()
[a_starts.size(), b_starts.size(), c_starts.size(),
 sup.restart_count(0), sup.restart_count(1), sup.restart_count(2)]
