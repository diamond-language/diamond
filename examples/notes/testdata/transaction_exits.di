# A return or break out of NoteStore#transaction rolls back, like an
# exception does. Prints the two exits, then 0 notes, then 1.
require "../lib/store"

# An in-memory database: nothing to clean up.
store = NoteStore.new(":memory:", "2026-01-01")

# First exit: `return` from inside the transaction block. The note added
# before the return must be rolled back.
def sneaky(store: NoteStore) -> String
  store.transaction() do
    store.add("inside", "x", [])
    return "left early"
  end
  "not reached"
end
puts(sneaky(store))

# Second exit: `break` out of the block, which also rolls back and makes
# `transaction` return the break's value.
got = store.transaction() do
  store.add("also inside", "y", [])
  break "broke out"
end
puts(got)

# Both inserts were rolled back, so there are 0 notes...
puts(store.count())

# ...and a normal transaction still commits, so now there is 1.
store.add("kept", "z", [])
puts(store.count())
