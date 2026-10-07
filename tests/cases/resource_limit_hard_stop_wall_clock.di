# Same as resource_limit_hard_stop_instructions, for the wall-clock budget.
def run()
  index = 0
  begin
    while true
      index = index + 1
    end
  rescue error: ResourceLimitError
    nil
  end
  while true
    index = index + 1
  end
end
run()
