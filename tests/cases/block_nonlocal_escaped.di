# A block called after the call or method it belongs to has finished can't
# break or return there; that's an ordinary, rescuable runtime error.
def keep(&blk) -> Callable = blk

def returner() -> Callable | Int
  keep() do
    return 1
  end
end

breaker = keep() do
  break 2
end

messages = []
[returner(), breaker].each() do |block|
  begin
    block()
  rescue e: Exception
    messages.push(e.message())
  end
end
messages
