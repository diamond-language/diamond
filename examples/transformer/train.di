# Trains a tiny transformer on a trivial fixed toy task -- predict
# "next id = id + 1" over a short sequence -- and prints the loss every
# few steps. This is the actual proof training works end to end (every
# Autograd.* backward formula was already checked individually against
# finite differences in gradcheck.di; this checks the *whole graph*,
# built from a real model, actually drives a real loss down): if the
# autodiff/optimizer wiring were wrong, loss would stay flat or diverge
# instead of dropping toward ~0.
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

# A very small model: vocabulary of 12 ids.
vocab_size = 12
d_model = 16
num_heads = 2
d_ff = 32
num_layers = 2
max_seq_len = 8

rng = SimpleRng.new(1)
model = TransformerModel.new(vocab_size, d_model, num_heads, d_ff, num_layers, max_seq_len, rng)

# Plain SGD with learning rate 0.05 over every trainable weight.
optimizer = SGD.new(model.parameters(), 0.05)

# The task: each input id should predict the NEXT id in 0..6. Inputs are
# 0..5 and targets are 1..6 (the same list shifted by one).
sequence = [0, 1, 2, 3, 4, 5, 6]
inputs = []
targets = []
i = 0

while i < sequence.length() - 1
  inputs.push(sequence[i])
  targets.push(sequence[i + 1])
  i += 1
end

puts("training on inputs=#{inputs} -> targets=#{targets}")

# The training loop: forward pass, loss, backward pass, weight update. The
# gradients are cleared first each time so steps do not accumulate.
steps = 200
step = 0

while step < steps
  optimizer.zero_grad!()
  logits = model.forward(inputs)
  loss = Loss.softmax_cross_entropy(logits, targets)
  backward!(loss)
  optimizer.step!()

  # Report every 20th step, so the loss curve is visible.
  if mod(step, 20) == 0
    puts("step #{step}: loss #{loss.tensor().get(0, 0)}")
  end
  step += 1
end

# After training: the loss once more, and what the model now predicts for
# each input (the highest-scoring id in that position's row of logits).
final_logits = model.forward(inputs)
final_loss = Loss.softmax_cross_entropy(final_logits, targets)
puts("final loss: #{final_loss.tensor().get(0, 0)}")

puts("predictions after training:")
i = 0
while i < inputs.length()
  row = i
  best_index = 0
  best_value = final_logits.tensor().get(row, 0)
  j = 1
  while j < vocab_size
    value = final_logits.tensor().get(row, j)
    if value > best_value
      best_value = value
      best_index = j
    end
    j += 1
  end
  puts("input #{inputs[i]} -> predicted #{best_index} (target #{targets[i]})")
  i += 1
end
