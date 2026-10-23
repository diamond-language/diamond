# DIAMOND_GRACE_MEMORY_BYTES replaces the default ceiling above the budget (a
# quarter of it, at least 4 MiB): a rescue clause that allocates about 200 KB
# fits the default but not 10,000 bytes.
def run()
  items = []
  index = 0
  begin
    while true
      items.push({"n": index})
      index = index + 1
    end
  rescue error: ResourceLimitError
    extra = []
    count = 0
    while count < 3000
      extra.push({"n": count})
      count = count + 1
    end
  end
  "finished"
end
run()
