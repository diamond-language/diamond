# The instruction budget bounds the whole process, not each thread: every
# worker below stays well under 400,000 instructions on its own, but together
# they pass it, so at least one of them is told so.
def worker()
  index = 0
  begin
    while index < 30000
      index = index + 1
    end
    "within"
  rescue error: ResourceLimitError
    "tripped"
  end
end

def run_workers(count)
  threads = []
  count.times() do |index|
    threads.push(Thread.new(worker))
  end
  threads.map() do |thread|
    thread.join()
  end
end

puts(run_workers(1).include?("tripped"))
puts(run_workers(4).include?("tripped"))
