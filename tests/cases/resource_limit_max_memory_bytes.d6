def run()
  items = []
  index = 0
  begin
    while true
      items.push({"n": index})
      index = index + 1
    end
  rescue error: ResourceLimitError
    return "caught"
  end
  "not caught"
end
run()
