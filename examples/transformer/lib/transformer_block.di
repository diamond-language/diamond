# One pre-norm transformer block (GPT-style: LayerNorm before each
# sub-layer, not after -- generally the more stable-to-train
# arrangement, though there's no training here to actually benefit
# from that): LN -> attention -> residual add, then LN -> feed-forward
# -> residual add.
class TransformerBlock
  attr_accessor attention, feed_forward, ln1_gamma, ln1_beta, ln2_gamma, ln2_beta, eps

  def initialize(d_model, num_heads, d_ff, rng)
    @attention = MultiHeadAttention.new(d_model, num_heads, rng)
    @feed_forward = FeedForward.new(d_model, d_ff, rng)
    @ln1_gamma = tensor_ones(1, d_model)
    @ln1_beta = Tensor.zeros(1, d_model)
    @ln2_gamma = tensor_ones(1, d_model)
    @ln2_beta = Tensor.zeros(1, d_model)
    @eps = 0.00001
  end

  def forward(x, causal)
    normalized1 = tensor_layernorm(x, @ln1_gamma, @ln1_beta, @eps)
    attn_out = @attention.forward(normalized1, causal)
    residual1 = tensor_add!(attn_out, x)

    normalized2 = tensor_layernorm(residual1, @ln2_gamma, @ln2_beta, @eps)
    ff_out = @feed_forward.forward(normalized2)
    tensor_add!(ff_out, residual1)
  end
end
