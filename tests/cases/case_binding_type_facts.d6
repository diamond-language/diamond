# A case-pattern binding gets the type facts of the element it binds --
# not those of whatever sits in register 0 (here, the `store` parameter),
# which used to make `check(seconds)` a bogus compile error.
class Store
end
def check(text: String) -> Bool = text.length() > 0
def handle(store: Store, line: String) -> String
  case line.split(" ")
  when ["SETEX", key, seconds] then if check(seconds) then "ok #{key}" else "empty" end
  when {"never": value} then "hash"
  else "other"
  end
end
[handle(Store.new(), "SETEX k 5"), handle(Store.new(), "GET k")]
