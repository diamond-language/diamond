# Stacks everything: embedding -> num_layers TransformerBlocks (causal,
# GPT-style decoder-only) -> final LayerNorm -> vocab-size output
# projection. #forward returns a Var of raw logits, not probabilities
# -- feed it to Loss.softmax_cross_entropy (loss.di) for training, or
# read raw logits/argmax yourself for inference (demo.di/benchmark.di
# do the latter). #parameters collects every trainable leaf Var, for
# an optimizer (optimizer.di) to walk once per training step.
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
    @final_ln_gamma = Var.leaf(tensor_ones(1, d_model))
    @final_ln_beta = Var.leaf(Tensor.zeros(1, d_model))
    @eps = 0.00001
    @output_proj = Linear.random(d_model, vocab_size, rng)
  end

  # token_ids: Array of Ints. Returns a Var (seq_len x vocab_size) of
  # raw logits.
  def forward(token_ids)
    x = @embedding.forward(token_ids)
    i = 0
    while i < @blocks.length()
      x = @blocks[i].forward(x, true)
      i += 1
    end
    normalized = Autograd.layernorm(x, @final_ln_gamma, @final_ln_beta, @eps)
    @output_proj.forward(normalized)
  end

  def parameters()
    params = []
    params.push(@embedding.token_table())
    params.push(@embedding.position_table())
    i = 0
    while i < @blocks.length()
      block = @blocks[i]
      attention = block.attention()
      feed_forward = block.feed_forward()
      params.push(attention.query_proj().weight())
      params.push(attention.query_proj().bias())
      params.push(attention.key_proj().weight())
      params.push(attention.key_proj().bias())
      params.push(attention.value_proj().weight())
      params.push(attention.value_proj().bias())
      params.push(attention.output_proj().weight())
      params.push(attention.output_proj().bias())
      params.push(feed_forward.up_proj().weight())
      params.push(feed_forward.up_proj().bias())
      params.push(feed_forward.down_proj().weight())
      params.push(feed_forward.down_proj().bias())
      params.push(block.ln1_gamma())
      params.push(block.ln1_beta())
      params.push(block.ln2_gamma())
      params.push(block.ln2_beta())
      i += 1
    end
    params.push(@final_ln_gamma)
    params.push(@final_ln_beta)
    params.push(@output_proj.weight())
    params.push(@output_proj.bias())
    params
  end
end
