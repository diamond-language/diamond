# A loop that only pushes onto one live array allocates nothing new, so the
# budget used to be checked only when something else allocated. Growing the
# array's own storage past the budget now trips it too.
def run()
  items = []
  index = 0
  begin
    while true
      items.push(index)
      index = index + 1
    end
  rescue error: ResourceLimitError
    return "caught"
  end
  "not caught"
end
run()
