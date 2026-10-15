# Native values (Time, Channel, Fiber, File, Regexp, ...) can be named in
# annotations, struct fields, unions, is, and is_a?. A struct checks its
# field types when constructed.
struct Event(at: Time, title: String)
end
def later(t: Time, days: Int) -> Time = t.days_from_now(days)
def queued(channel: Channel) -> Int = channel.size()
def maybe(flag: Bool) -> Time | Nil = if flag then Time.utc(2026, 1, 1, 0, 0, 0) else nil end
def opaque(value) = value
event = Event.new(Time.utc(2026, 1, 1, 0, 0, 0), "launch")
failures = []
begin
  later(opaque(Channel.new(1)), 1)
rescue error: TypeError
  failures.push(error.message())
end
begin
  Event.new(opaque("not a time"), "x")
rescue error: TypeError
  failures.push(error.message())
end
[later(event.at(), 2).day(), queued(Channel.new(2)), event.at() is Time,
 event.at().is_a?(Time), event.at() is Fiber, "text" is Time, maybe(false) == nil, failures]
