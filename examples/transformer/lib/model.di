# Stacks everything: embedding -> num_layers TransformerBlocks (causal,
# GPT-style decoder-only) -> final LayerNorm -> vocab-size output
# projection. #forward returns raw logits, not probabilities -- softmax
# them yourself (or just argmax, like demo.di does) depending on what
# you need.
class TransformerModel
  attr_accessor embedding, blocks, final_ln_gamma, final_ln_beta, eps, output_proj

  def initialize(vocab_size, d_model, num_heads, d_ff, num_layers, max_seq_len, rng)
    @embedding = Embedding.new(vocab_size, d_model, max_seq_len, rng)
    @blocks = []
    i = 0
    while i < num_layers
      @blocks.push(TransformerBlock.new(d_model, num_heads, d_ff, rng))
      i += 1
    end
    @final_ln_gamma = tensor_ones(1, d_model)
    @final_ln_beta = Tensor.zeros(1, d_model)
    @eps = 0.00001
    @output_proj = Linear.random(d_model, vocab_size, rng)
  end

  # token_ids: Array of Ints. Returns (seq_len x vocab_size) logits.
  def forward(token_ids)
    x = @embedding.forward(token_ids)
    i = 0
    while i < @blocks.length()
      x = @blocks[i].forward(x, true)
      i += 1
    end
    normalized = tensor_layernorm(x, @final_ln_gamma, @final_ln_beta, @eps)
    @output_proj.forward(normalized)
  end
end
