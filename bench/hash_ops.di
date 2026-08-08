# Hash insert/lookup in a loop -- exercises INDEX_GET/INDEX_SET's Hash
# branch. hash_find (src/vm.c) is a genuine O(n) linear scan over
# entries, not a real hash table, so both insert and lookup here are
# O(n) per operation -- this benchmark is deliberately sized small
# (5000, not e.g. 50000) since the insert phase is O(n^2) overall and
# grows fast; the O(n) cost per op is exactly the point being measured.
def run()
  values = {}
  index = 0
  while index < 5000
    values[index] = index * 2
    index = index + 1
  end
  total = 0
  index = 0
  while index < 5000
    total = total + values[index]
    index = index + 1
  end
  total
end
run()
