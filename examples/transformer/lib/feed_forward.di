# Position-wise feed-forward: d_model -> d_ff -> d_model with GELU in
# between (d_ff is conventionally 4x d_model, e.g. BERT-base's
# 768->3072->768) -- the other half of a transformer block's FLOPs,
# alongside attention.
class FeedForward
  attr_accessor up_proj, down_proj

  def initialize(d_model, d_ff, rng)
    @up_proj = Linear.random(d_model, d_ff, rng)
    @down_proj = Linear.random(d_ff, d_model, rng)
  end

  def forward(x)
    hidden = @up_proj.forward(x)
    tensor_gelu!(hidden)
    @down_proj.forward(hidden)
  end
end
