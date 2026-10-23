# Clearing and refilling Hashes of heap values while every allocation
# collects: nothing live may be freed, nothing cleared may be kept alive by
# a stale entry, and the Hash must stay consistent throughout.
holder = {}
round = 0
while round < 20
  holder.clear()
  i = 0
  while i < 30
    holder["key#{i}"] = ["value#{i}", {"round": round}]
    i += 1
  end
  round += 1
end
puts(holder.length())
puts(holder["key29"][0])
puts(holder["key0"][1]["round"])
holder.clear()
puts(holder.length())
