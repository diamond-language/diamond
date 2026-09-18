# Hash insert/lookup in a loop -- exercises INDEX_GET/INDEX_SET's Hash
# branch. hash_find (src/vm.c) is a real open-addressing hash table
# (hash_value + a separate buckets[] index array, linear probing,
# rehashed at a 0.75 load factor, entries[] itself never reordered so
# insertion order and "update doesn't move position" both hold across
# any number of rehashes) -- both insert and lookup are O(1) average
# case, not O(n). This comment used to say otherwise: Hash really was
# a plain O(n) linear scan (`hash_find` did nothing but loop over
# every entry comparing keys) until 2026-08-07 (`15d8d868`, "Replace
# Hash's O(n) linear scan with a real hash table"), but this file's
# own comment and sizing were never updated afterward -- confirmed
# stale (2026-09-18) by a direct scaling check: 100x more entries
# (5,000 -> 500,000) cost only ~10x more total time for insert+lookup,
# not the ~10,000x a real O(n) per-op / O(n^2) overall scan would
# produce. Sized at 200,000 now that there's no O(n^2) insert-phase
# blowup to avoid.
def run()
  values = {}
  index = 0
  while index < 200000
    values[index] = index * 2
    index = index + 1
  end
  total = 0
  index = 0
  while index < 200000
    total = total + values[index]
    index = index + 1
  end
  total
end
run()
