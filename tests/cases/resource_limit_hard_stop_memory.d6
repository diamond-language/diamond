# Same, for the memory budget: rescuing the first ResourceLimitError does not
# lift the cap, and allocating past its grace ceiling ends the program.
def run()
  items = []
  index = 0
  begin
    while true
      items.push({"n": index})
      index = index + 1
    end
  rescue error: ResourceLimitError
    nil
  end
  while true
    items.push({"n": index})
    index = index + 1
  end
end
run()
