# A mixin: `include Observable` copies these methods into the including
# class, so any class can announce events to whoever subscribed. The
# listener list lives in the including object's own @listeners, created on
# first use so no `initialize` cooperation is needed.
module Observable
  # Register a callback taking (event, details). Returns self so calls can be
  # chained.
  def subscribe(listener)
    @listeners = [] if @listeners == nil
    @listeners.push(listener)
    self
  end

  # How many callbacks are registered.
  def listener_count() -> Int
    if @listeners == nil then 0 else @listeners.length() end
  end

  # Call every listener with the event. Does nothing if nobody subscribed.
  def announce(event: Symbol, details: Array)
    return nil if @listeners == nil
    @listeners.each() do |listener| listener(event, details) end
    nil
  end
end
