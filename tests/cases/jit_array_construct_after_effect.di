# A failing return guard after array allocation must propagate without
# replaying the hash mutation, and pop the published allocation frame.
def after_array(log: Hash, value) -> Int
  log["calls"] = log["calls"] + 1
  items = [value]
  items[0]
end

def run()
  log = {"calls": 0}
  begin
    after_array(log, "wrong")
  rescue error: TypeError
    nil
  end
  log["calls"]
end

run()
