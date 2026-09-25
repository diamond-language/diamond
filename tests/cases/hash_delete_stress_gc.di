# Hash#delete shifts later entries down one slot. An old Hash tracks young
# values by 64-entry GC card, so a young value that a delete moves back
# across a card boundary must still be found by the next minor collection.
# `shift_in` appends a fresh string at index 64 (card 1) and deletes the
# first key, moving the string to index 63 (card 0). Once it returns, the
# Hash is the string's only reference, and the next allocation collects
# (run with DIAMOND_STRESS_MINOR_GC).
def shift_in(table, key)
  table[key] = "young " + key.to_s()
  table.delete(table.key_at(0))
  nil
end

table = {}
64.times() do |i| table[i] = i end
warmup = []
20.times() do |i| warmup.push("promote the table #{i}") end
40.times() do |i|
  shift_in(table, 100 + i)
  churn = []
  20.times() do |j| churn.push("churn #{i} #{j}") end
end
ok = true
40.times() do |i|
  ok = false if table[100 + i] != "young #{100 + i}"
end
[ok, table.length()]
