# Token embedding (vocab_size x d_model lookup table) + learned
# positional embedding (max_seq_len x d_model, indexed by position, not
# sinusoidal), summed -- GPT-style, not the sinusoidal encoding the
# original "Attention Is All You Need" paper used. Both tables are
# trainable Var leaves; Autograd.embedding scatters gradient back into
# the specific rows that were looked up.
class Embedding
  attr_accessor token_table, position_table, d_model

  def initialize(vocab_size, d_model, max_seq_len, rng)
    @d_model = d_model
    @token_table = Var.leaf(tensor_random(vocab_size, d_model, rng, 1.0 / sqrt(d_model)))
    @position_table = Var.leaf(tensor_random(max_seq_len, d_model, rng, 1.0 / sqrt(d_model)))
  end

  # token_ids: a plain Array of Ints, each < vocab_size and < max_seq_len
  # in position (i.e. token_ids.length() <= max_seq_len). Returns a Var
  # (seq_len x d_model).
  def forward(token_ids)
    Autograd.embedding(@token_table, @position_table, token_ids)
  end
end
