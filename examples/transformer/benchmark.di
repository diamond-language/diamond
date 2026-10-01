# A GPT-2-small-ish scale forward pass (not GPT-2's real vocab/dims,
# just in the same ballpark) -- demo.di proves correctness/wiring at
# toy scale, this one is for a real throughput number at a size where
# the underlying Tensor#matmul work actually matters.
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

# The model's size: 6 layers of 512-wide, 8-head blocks, over a made-up
# 8000-token vocabulary (nothing here is trained: the numbers only matter for
# timing).
vocab_size = 8000
d_model = 512
num_heads = 8
d_ff = 2048
num_layers = 6
max_seq_len = 128
seq_len = 64


# Build the model (timed separately: allocating and filling the weights is
# real work at this size).
rng = SimpleRng.new(7)
t_build0 = Time.monotonic()
model = TransformerModel.new(vocab_size, d_model, num_heads, d_ff, num_layers, max_seq_len, rng)
t_build1 = Time.monotonic()


# A fixed pseudo-random token sequence, so every run measures the same work.
token_ids = []
i = 0

while i < seq_len
  token_ids.push(mod(i * 97 + 13, vocab_size))
  i += 1
end


# One timed forward pass.
t0 = Time.monotonic()
logits = model.forward(token_ids).tensor()
t1 = Time.monotonic()

puts("build: #{t_build1 - t_build0}s")
puts("model: vocab=#{vocab_size} d_model=#{d_model} heads=#{num_heads} d_ff=#{d_ff} layers=#{num_layers}, seq_len=#{seq_len}")
puts("forward pass: #{logits.rows()}x#{logits.cols()} logits in #{t1 - t0}s")
