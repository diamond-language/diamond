# Multi-head self-attention. Q/K/V/output are each a full d_model x
# d_model Linear projection; per-head splitting happens *after*
# projecting (tensor_columns slices out each head's own head_dim-wide
# column range), matching how a real implementation's single big
# matmul-then-reshape works -- just without the reshape, since Tensor
# is 2D-only here (see this example's own README "Scope" section).
class MultiHeadAttention
  attr_accessor num_heads, head_dim, d_model, query_proj, key_proj, value_proj, output_proj

  # Caller's responsibility: d_model must be divisible by num_heads
  # (unenforced here -- a scaffold, not a hardened library).
  def initialize(d_model, num_heads, rng)
    @d_model = d_model
    @num_heads = num_heads
    @head_dim = d_model / num_heads
    @query_proj = Linear.random(d_model, d_model, rng)
    @key_proj = Linear.random(d_model, d_model, rng)
    @value_proj = Linear.random(d_model, d_model, rng)
    @output_proj = Linear.random(d_model, d_model, rng)
  end

  # x: (seq_len x d_model). `causal`: when true, position i can only
  # attend to positions <= i (the GPT-style decoder-only constraint) --
  # applied as a large negative bias added to masked scores before
  # softmax (drives their post-softmax weight to ~0), the simplest
  # correct approach rather than the packed/optimized masking a real
  # implementation would use.
  def forward(x, causal)
    seq_len = x.rows()
    q = @query_proj.forward(x)
    k = @key_proj.forward(x)
    v = @value_proj.forward(x)

    head_outputs = []
    head = 0
    while head < @num_heads
      start = head * @head_dim
      q_head = tensor_columns(q, start, @head_dim)
      k_head = tensor_columns(k, start, @head_dim)
      v_head = tensor_columns(v, start, @head_dim)

      scores = q_head.matmul(k_head.transpose())
      tensor_scale!(scores, 1.0 / sqrt(@head_dim))

      if causal
        i = 0
        while i < seq_len
          j = i + 1
          while j < seq_len
            scores.set(i, j, 0.0 - 1000000000.0)
            j += 1
          end
          i += 1
        end
      end

      tensor_row_softmax!(scores)
      head_outputs.push(scores.matmul(v_head))
      head += 1
    end

    concatenated = tensor_concat_columns(head_outputs)
    @output_proj.forward(concatenated)
  end
end
