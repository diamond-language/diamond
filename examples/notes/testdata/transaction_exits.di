# A return or break out of NoteStore#transaction rolls back, like an
# exception does. Prints the two exits, then 0 notes, then 1.
require "../lib/store"
store = NoteStore.new(":memory:", "2026-01-01")
def sneaky(store: NoteStore) -> String
  store.transaction() do
    store.add("inside", "x", [])
    return "left early"
  end
  "not reached"
end
puts(sneaky(store))
got = store.transaction() do
  store.add("also inside", "y", [])
  break "broke out"
end
puts(got)
puts(store.count())
store.add("kept", "z", [])
puts(store.count())
