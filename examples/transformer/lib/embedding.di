# Token embedding (vocab_size x d_model lookup table) + learned
# positional embedding (max_seq_len x d_model, indexed by position, not
# sinusoidal), summed -- GPT-style, not the sinusoidal encoding the
# original "Attention Is All You Need" paper used.
class Embedding
  attr_accessor token_table, position_table, d_model

  def initialize(vocab_size, d_model, max_seq_len, rng)
    @d_model = d_model
    @token_table = tensor_random(vocab_size, d_model, rng, 1.0 / sqrt(d_model))
    @position_table = tensor_random(max_seq_len, d_model, rng, 1.0 / sqrt(d_model))
  end

  # token_ids: a plain Array of Ints, each < vocab_size and < max_seq_len
  # in position (i.e. token_ids.length() <= max_seq_len). Returns
  # (seq_len x d_model).
  def forward(token_ids)
    seq_len = token_ids.length()
    result = Tensor.zeros(seq_len, @d_model)
    i = 0
    while i < seq_len
      token_id = token_ids[i]
      j = 0
      while j < @d_model
        result.set(i, j, @token_table.get(token_id, j) + @position_table.get(i, j))
        j += 1
      end
      i += 1
    end
    result
  end
end
