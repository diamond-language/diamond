def run()
  index = 0
  begin
    while true
      index = index + 1
    end
  rescue error: ResourceLimitError
    return "caught"
  end
  "not caught"
end
run()
