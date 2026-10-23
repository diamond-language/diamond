# The grace allowance is there so a rescue clause can do real work: this one
# runs a few thousand instructions of cleanup after the budget trips and the
# program then finishes normally.
def run()
  index = 0
  begin
    while true
      index = index + 1
    end
  rescue error: ResourceLimitError
    total = 0
    cleanup = 0
    while cleanup < 5000
      total = total + cleanup
      cleanup = cleanup + 1
    end
    return "cleaned #{total}"
  end
  "not caught"
end
run()
