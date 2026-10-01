require "../lib/cancellation"

def check(value, message)
  unless value then raise message end
end

def waiting(token, ready, cleaned)
  begin
    ready.send(true)
    token.receive(Channel.new(1))
  ensure
    cleaned.send("cleaned")
  end
end

def sending(token, ready, full, cleaned)
  begin
    ready.send(true)
    token.send(full, 2)
  ensure
    cleaned.send("cleaned")
  end
end

def stop_after_ready(scope, ready)
  token = scope.token()
  token.receive(ready)
  scope.cancel()
end

source = Cancellation::Source.new()
child = Cancellation::Source.new(source.token())
child.cancel()
source.token().checkpoint()
begin
  child.token().checkpoint()
  raise "child not cancelled"
rescue error: Cancellation::Cancelled
end
parent_child = Cancellation::Source.new(source.token())
source.cancel()
source.cancel()
begin
  parent_child.token().checkpoint()
  raise "parent cancellation lost"
rescue error: Cancellation::Cancelled
end

[0, 0.02].each() do |seconds|
  deadline = Cancellation::Source.new(nil, seconds)
  cleaned = false
  begin
    begin
      deadline.token().sleep(1)
    ensure
      cleaned = true
    end
    raise "deadline did not expire"
  rescue error: Cancellation::DeadlineExceeded
    check(cleaned, "deadline skipped ensure")
  end
end

ready = Channel.new(1)
cleaned = Channel.new(1)
scope = Cancellation::Scope.new(nil, 2)
scope.spawn(waiting, ready, cleaned)
scope.token().receive(ready)
scope.close()
check(cleaned.receive() == "cleaned", "receive cancellation skipped ensure")
scope.close()

full = Channel.new(1)
full.send(1)
scope = Cancellation::Scope.new(nil, 2)
scope.spawn(sending, ready, full, cleaned)
scope.token().receive(ready)
scope.close()
check(cleaned.receive() == "cleaned", "send cancellation skipped ensure")
check(full.receive() == 1, "cancelled send changed channel")

channel = Channel.new(1)
source = Cancellation::Source.new()
source.token().send(channel, 9)
check(source.token().receive(channel) == 9, "channel value lost")
channel.close()
check(source.token().receive(channel) == nil, "closed channel EOF lost")
begin
  source.token().send(channel, 1)
  raise "closed send succeeded"
rescue error: IOError
end

def fail_task(token, ready)
  token.receive(ready)
  raise RuntimeError.new("child failure")
end

def failing_body(scope)
  ready = Channel.new(1)
  cleaned = Channel.new(1)
  scope.spawn(waiting, ready, cleaned)
  scope.spawn(fail_task, ready)
end
begin
  Cancellation.scope(failing_body, 2)
  raise "child failure swallowed"
rescue error: RuntimeError
  check(error.message() == "child failure", "wrong child error")
end

def body_failure(scope)
  ready = Channel.new(1)
  cleaned = Channel.new(1)
  scope.spawn(waiting, ready, cleaned)
  scope.token().receive(ready)
  raise RuntimeError.new("body failure")
end
begin
  Cancellation.scope(body_failure, 2)
  raise "body failure swallowed"
rescue error: RuntimeError
  check(error.message() == "body failure", "wrong body error")
end

def normal_task(token, out)
  token.checkpoint()
  out.send(7)
end

def normal_body(scope)
  out = Channel.new(1)
  scope.spawn(normal_task, out)
  scope.token().receive(out)
end
check(Cancellation.scope(normal_body, 2) == 7, "scope result lost")

[-1, "bad"].each() do |duration|
  begin
    Cancellation::Source.new(nil, duration)
    raise "invalid duration accepted"
  rescue error: ArgumentError
  end
end
def nil_failure(scope)
  raise nil
end
caught = false
begin
  Cancellation.scope(nil_failure)
rescue error
  caught = true
  check(error == nil, "raised nil changed")
end
check(caught, "raised nil swallowed")
def supervised(token, attempts, ready, cleaned)
  first = false
  begin
    attempts.try_receive()
  rescue error: WouldBlockError
    first = true
  end
  if first
    attempts.send(true)
    raise RuntimeError.new("restart once")
  end
  begin
    ready.send(true)
    token.receive(Channel.new(1))
  rescue error: Cancellation::Cancelled
    nil
  ensure
    cleaned.send(true)
  end
end
source = Cancellation::Source.new(nil, 2)
supervisor = Supervisor.new()
ready = Channel.new(1)
cleaned = Channel.new(1)
supervisor.add_child(supervised, source.token(), Channel.new(1), ready, cleaned)
source.token().receive(ready)
source.cancel()
supervisor.stop()
check(supervisor.restart_count(0) == 1, "unexpected supervisor restarts")
check(cleaned.receive() == true, "supervisor cleanup lost")

# One close broadcasts; cancellation is not a consumable queue message.
source = Cancellation::Source.new()
ready = Channel.new(4)
cleaned = Channel.new(4)
threads = []
4.times() do |i|
  threads.push(Thread.new(Cancellation.task, source, waiting, [ready, cleaned]))
end
4.times() do |i| ready.receive() end
source.cancel()
threads.each() do |thread| thread.join() end
check(cleaned.size() == 4, "cancellation did not reach every child")
# A parent's earlier deadline governs a child native wait.
parent = Cancellation::Source.new(nil, 0.02)
child = Cancellation::Source.new(parent.token(), 10)
begin
  child.token().receive(Channel.new(1))
  raise "ancestor deadline ignored"
rescue error: Cancellation::DeadlineExceeded
end

# A copied child's token must wake when an ancestor is cancelled.
parent = Cancellation::Source.new()
child = Cancellation::Source.new(parent.token())
ready = Channel.new(1)
cleaned = Channel.new(1)
thread = Thread.new(Cancellation.task, child, waiting, [ready, cleaned])
ready.receive()
parent.cancel()
thread.join()
check(cleaned.receive() == "cleaned", "ancestor cancellation lost")
puts("cancellation tests passed")
exit(0)
