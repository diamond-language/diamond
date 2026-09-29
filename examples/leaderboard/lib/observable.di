# A mixin: `include Observable` copies these methods into the including
# class, so any class can announce events to whoever subscribed. The
# listener list lives in the including object's own @listeners, created on
# first use so no `initialize` cooperation is needed.
module Observable
  def subscribe(listener)
    @listeners = [] if @listeners == nil
    @listeners.push(listener)
    self
  end

  def listener_count() -> Int
    if @listeners == nil then 0 else @listeners.length() end
  end

  def announce(event: Symbol, details: Array)
    return nil if @listeners == nil
    @listeners.each() do |listener| listener(event, details) end
    nil
  end
end
