# run! exit=1 contains=wrong number of arguments
class BlockTarget
  def transform(value, callback: Callable[1])
    callback(value)
  end
end

class BlockProxy
  def initialize(target)
    @target = target
  end

  delegate transform(value, &block), to: @target
end

BlockProxy.new(BlockTarget.new()).transform(6)
