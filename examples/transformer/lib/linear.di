# y = x.matmul(weight) + bias, the one building block everything else
# (attention's Q/K/V/output projections, the feed-forward's up/down
# projections, the final vocab projection) is made of.
class Linear
  attr_accessor weight, bias

  def initialize(weight, bias)
    @weight = weight
    @bias = bias
  end

  # x: (rows x in_features). weight: (in_features x out_features).
  # bias: (1 x out_features). Returns (rows x out_features).
  def forward(x)
    output = x.matmul(@weight)
    tensor_add_bias!(output, @bias)
    output
  end

  # 1/sqrt(in_features) uniform scale -- a loose approximation of
  # Xavier/Glorot init, not rigorous (there's no training here to
  # actually benefit from a carefully-tuned init distribution); good
  # enough to avoid a degenerate all-zero or blown-up forward pass.
  def self.random(in_features, out_features, rng)
    scale = 1.0 / sqrt(in_features)
    weight = tensor_random(in_features, out_features, rng, scale)
    bias = Tensor.zeros(1, out_features)
    Linear.new(weight, bias)
  end
end
