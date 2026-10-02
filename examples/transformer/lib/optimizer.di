# Plain SGD (no momentum, no Adam-style per-parameter adaptive rates --
# a real transformer would normally train with Adam/AdamW, but plain
# SGD is enough to prove the whole autograd graph actually produces
# useful gradients, which is what train.di is checking for). Neither
# method here needs `closure`, so both are ordinary instance methods.
class SGD
  attr_accessor parameters, learning_rate

  def initialize(parameters, learning_rate)
    @parameters = parameters
    @learning_rate = learning_rate
  end

  # One gradient-descent step: move every weight against its gradient,
  # scaled by the learning rate (w = w - lr * dL/dw).
  def step!()
    i = 0

    while i < @parameters.length()
      param = @parameters[i]
      j = 0
      while j < param.tensor().rows()
        k = 0
        while k < param.tensor().cols()
          updated = param.tensor().get(j, k) - @learning_rate * param.grad().get(j, k)
          param.tensor().set(j, k, updated)
          k += 1
        end
        j += 1
      end
      i += 1
    end
  end

  # Clear every gradient. Must be called before each forward pass, or
  # gradients from the previous step would be added to this one's.
  def zero_grad!()
    i = 0
    while i < @parameters.length()
      @parameters[i].zero_grad!()
      i += 1
    end
  end
end
