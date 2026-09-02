# A node in the reverse-mode autodiff graph: wraps one Tensor (the
# forward value), its accumulated gradient (same shape, starts at
# zero), the Vars that produced it (`parents`), and the Callable that
# knows how to push this node's own `grad` back into its parents'
# `grad` (`backward_fn`, nil for a leaf/constant -- nothing to push
# further back). Autograd.* (autograd.di) builds these; `backward!`
# (also autograd.di) walks the graph.
class Var
  attr_accessor tensor, grad, parents, backward_fn, requires_grad

  def initialize(tensor, parents, requires_grad)
    @tensor = tensor
    @grad = Tensor.zeros(tensor.rows(), tensor.cols())
    @parents = parents
    @backward_fn = nil
    @requires_grad = requires_grad
  end

  # A trainable leaf -- an optimizer will update its .tensor from its
  # .grad (optimizer.di).
  def self.leaf(tensor)
    Var.new(tensor, [], true)
  end

  # A non-trainable input (e.g. this step's token ids' looked-up
  # embedding row before any further op -- the row itself is a leaf
  # elsewhere; this is for values that genuinely never need a gradient,
  # like a fixed mask).
  def self.constant(tensor)
    Var.new(tensor, [], false)
  end

  def zero_grad!()
    @grad = Tensor.zeros(@tensor.rows(),@tensor.cols())
  end
end
