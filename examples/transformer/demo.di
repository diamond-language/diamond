# Builds a small, randomly-initialized (untrained) GPT-style
# decoder-only transformer and runs one real forward pass through it --
# proves the whole pipeline (embedding -> N attention+feed-forward
# blocks -> output projection) actually wires together and produces
# real logits, on top of examples/../src Tensor. #forward returns a
# Var (var.di), not a raw Tensor -- see train.di for the trainable
# side of this same model (this script never calls backward!, so the
# autograd graph #forward also builds here is simply unused, harmless
# overhead). See this directory's own README.md for what is and isn't
# in scope.
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

vocab_size = 50
d_model = 32
num_heads = 4
d_ff = 128
num_layers = 2
max_seq_len = 16

rng = SimpleRng.new(42)
model = TransformerModel.new(vocab_size, d_model, num_heads, d_ff, num_layers, max_seq_len, rng)

token_ids = [3, 17, 8, 41, 2, 9]

t0 = Time.monotonic()
logits = model.forward(token_ids).tensor()
t1 = Time.monotonic()

puts("model: vocab=#{vocab_size} d_model=#{d_model} heads=#{num_heads} d_ff=#{d_ff} layers=#{num_layers}")
puts("input: #{token_ids.length()} tokens -> logits #{logits.rows()}x#{logits.cols()}")
puts("forward pass took #{t1 - t0}s")

last_row = logits.rows() - 1
top_value = logits.get(last_row, 0)
top_index = 0
j = 1
while j < logits.cols()
  value = logits.get(last_row, j)
  if value > top_value
    top_value = value
    top_index = j
  end
  j += 1
end
puts("argmax next-token prediction (untrained weights, not a real prediction): token #{top_index}, logit #{top_value}")
