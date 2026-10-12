# DIAMOND_GRACE_INSTRUCTIONS shrinks the cleanup allowance: the cleanup loop
# below fits the default 1,000,000 instructions but not 500.
def run()
  index = 0
  begin
    while true
      index = index + 1
    end
  rescue error: ResourceLimitError
    cleanup = 0
    while cleanup < 5000
      cleanup = cleanup + 1
    end
  end
  "finished"
end
run()
