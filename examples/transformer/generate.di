# Loads a checkpoint (train_corpus.di) and generates text continuing a
# prompt, one byte at a time -- greedy argmax (simplest, deterministic;
# real sampling with temperature/top-k is a natural follow-up, not
# needed to prove the checkpoint round-trip and generation loop work).
#
# Usage: ../../build/diamond generate.di <checkpoint_path> <prompt> [num_tokens]
require "./lib/rng"
require "./lib/tensor_ops"
require "./lib/var"
require "./lib/autograd"
require "./lib/linear"
require "./lib/attention"
require "./lib/feed_forward"
require "./lib/transformer_block"
require "./lib/embedding"
require "./lib/model"
require "./lib/loss"
require "./lib/optimizer"
require "./lib/tokenizer"
require "./lib/checkpoint"

checkpoint_path = ARGV[0]
prompt = ARGV[1]
if checkpoint_path == nil || prompt == nil
  puts("usage: diamond generate.di <checkpoint_path> <prompt> [num_tokens]")
  exit(1)
end
num_tokens = if ARGV[2] == nil then 200 else ARGV[2].to_i() end

# Must match train_corpus.di's own config exactly -- Checkpoint.load!
# only checks the parameter *count*, not each Tensor's shape, so a
# mismatched config here would either error inside Tensor.from_array or
# silently load wrong-shaped weights.
d_model = 128
num_heads = 4
d_ff = 512
num_layers = 4
seq_len = 64
max_seq_len = seq_len
vocab_size = ByteTokenizer.vocab_size()

rng = SimpleRng.new(1)
model = TransformerModel.new(vocab_size, d_model, num_heads, d_ff, num_layers, max_seq_len, rng)
Checkpoint.load!(model, checkpoint_path)

token_ids = ByteTokenizer.encode(prompt)

step = 0
while step < num_tokens
  # The model has no K/V cache -- every step re-runs the full forward
  # pass over the whole context so far (also why the context is
  # clamped to the last max_seq_len tokens below: this scaffold's
  # positional embedding table only has max_seq_len rows). Fine for a
  # demo; a real implementation would cache across steps instead of
  # recomputing them.
  context = token_ids
  if context.length() > max_seq_len
    trimmed = []
    start = context.length() - max_seq_len
    i = start
    while i < context.length()
      trimmed.push(context[i])
      i += 1
    end
    context = trimmed
  end

  logits = model.forward(context).tensor()
  last_row = logits.rows() - 1
  best_index = 0
  best_value = logits.get(last_row, 0)
  j = 1
  while j < vocab_size
    value = logits.get(last_row, j)
    if value > best_value
      best_value = value
      best_index = j
    end
    j += 1
  end
  token_ids.push(best_index)
  step += 1
end

puts(ByteTokenizer.decode(token_ids))
