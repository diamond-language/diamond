# run! exit=1 contains=a variadic delegate parameter must be last
class BlockProxy
  delegate transform(*values, &block), to: @target
end
