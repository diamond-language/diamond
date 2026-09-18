def hydrate(hash)
  index = 0
  count = 0
  while index < 5
    v = hash["value"]
    count = count + 1
    index = index + 1
  end
  count
end

def run()
  h = {"value": 3}
  hydrate(h)
end
puts(run())
