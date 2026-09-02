# y = x.matmul(weight) + bias, the one building block everything else
# (attention's Q/K/V/output projections, the feed-forward's up/down
# projections, the final vocab projection) is made of. weight/bias are
# both trainable Var leaves now (Autograd.matmul/#add_bias push
# gradient into them during backward!).
class Linear
  attr_accessor weight, bias

  def initialize(weight, bias)
    @weight = weight
    @bias = bias
  end

  # x: Var (rows x in_features). Returns Var (rows x out_features).
  def forward(x)
    Autograd.add_bias(Autograd.matmul(x, @weight), @bias)
  end

  # 1/sqrt(in_features) uniform scale -- a loose approximation of
  # Xavier/Glorot init, not rigorous; good enough to avoid a
  # degenerate all-zero or blown-up forward pass, and training itself
  # is what actually shapes these weights from here.
  def self.random(in_features, out_features, rng)
    scale = 1.0 / sqrt(in_features)
    weight = tensor_random(in_features, out_features, rng, scale)
    bias = Tensor.zeros(1, out_features)
    Linear.new(Var.leaf(weight), Var.leaf(bias))
  end
end
