# DIAMOND_GRACE_INSTRUCTIONS widens it: this much work after the rescue is past
# the default 1,000,000 instructions, and runs to completion with a larger one.
def run()
  index = 0
  begin
    while true
      index = index + 1
    end
  rescue error: ResourceLimitError
    cleanup = 0
    while cleanup < 400000
      cleanup = cleanup + 1
    end
  end
  "finished"
end
run()
