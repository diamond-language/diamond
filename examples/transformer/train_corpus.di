# Trains the transformer on a real text corpus assembled from every
# .txt/.md/.json file under a folder (recursively -- corpus.di handles
# both plain text and TinyStories-shaped JSON) -- the practical
# counterpart to train.di's own hardcoded toy sequence. Byte-level
# tokenized (tokenizer.di), chopped into fixed-length windows
# (dataset.di), one window per SGD step (still no batching dimension --
# see the README's own Scope section), checkpointed to disk after
# every epoch (checkpoint.di) so training can actually be used for
# something afterward (generate.di loads a checkpoint and samples text
# from it).
#
# Usage: ../../build/diamond train_corpus.di <folder> [checkpoint_path] [max_files] [max_stories]
#
# max_files/max_stories (both default to a small, fast-to-iterate-on
# value -- see corpus.di's own comment on why parsing is the real cost
# at TinyStories' actual scale, ~330ms/MB measured directly): pass a
# larger number, or the literal "all", once the model/hyperparameters
# look right on a quick run and you're ready to actually commit the
# time to a bigger corpus.
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
require "./lib/corpus"
require "./lib/dataset"
require "./lib/checkpoint"

folder = ARGV[0]
if folder == nil
  puts("usage: diamond train_corpus.di <folder> [checkpoint_path]")
  exit(1)
end
checkpoint_path = if ARGV[1] == nil then "checkpoint.json" else ARGV[1] end
max_files = if ARGV[2] == nil then 26 elsif ARGV[2] == "all" then nil else ARGV[2].to_i() end
max_stories = if ARGV[3] == nil then 50 elsif ARGV[3] == "all" then nil else ARGV[3].to_i() end

# Small enough to train at a reasonable pace on a modest corpus with
# plain SGD on CPU -- not tuned for quality, a starting point to adjust
# once real training-time/loss numbers are in hand. One training step
# at this exact config measured ~4.2s on this machine (Var/Autograd's
# own per-op bookkeeping -- allocation, closures, GC -- not raw Tensor
# math, which is now native throughout; see autograd.di's own history
# for the ops that got moved to C and how much that did/didn't help)
# -- the estimated-time printout below multiplies that measured
# constant by the real window/epoch count so a change to any of these
# (or to max_stories above) comes with an honest number attached
# instead of an unannounced multi-hour run.
d_model = 128
num_heads = 4
d_ff = 512
num_layers = 4
seq_len = 64
max_seq_len = seq_len
learning_rate = 0.01
epochs = 3
stride = seq_len
measured_seconds_per_step = 4.2

vocab_size = ByteTokenizer.vocab_size()

text = Corpus.load(folder, [".txt", ".md", ".json"], max_files, max_stories)
if text.length() == 0
  puts("no .txt/.md/.json files found under #{folder}")
  exit(1)
end

token_ids = ByteTokenizer.encode(text)
examples = Dataset.windows(token_ids, seq_len, stride)
if examples.length() == 0
  puts("corpus is shorter than seq_len (#{seq_len}) + 1 tokens -- nothing to train on")
  exit(1)
end
puts("#{examples.length()} training example(s) (seq_len=#{seq_len}, stride=#{stride})")
estimated_hours = examples.length() * epochs * measured_seconds_per_step / 3600.0
puts("estimated total training time: ~#{estimated_hours} hour(s) for #{epochs} epoch(s) (based on a ~#{measured_seconds_per_step}s/step measurement on this machine -- adjust max_stories/epochs above if this is too long)")

rng = SimpleRng.new(1)
model = TransformerModel.new(vocab_size, d_model, num_heads, d_ff, num_layers, max_seq_len, rng)
optimizer = SGD.new(model.parameters(), learning_rate)

puts("model: vocab=#{vocab_size} d_model=#{d_model} heads=#{num_heads} d_ff=#{d_ff} layers=#{num_layers} seq_len=#{seq_len}")

epoch = 0
while epoch < epochs
  total_loss = 0.0
  example_index = 0
  while example_index < examples.length()
    pair = examples[example_index]
    inputs = pair[0]
    targets = pair[1]

    optimizer.zero_grad!()
    logits = model.forward(inputs)
    loss = Loss.softmax_cross_entropy(logits, targets)
    backward!(loss)
    optimizer.step!()

    total_loss += loss.tensor().get(0, 0)
    if mod(example_index, 20) == 0
      puts("epoch #{epoch} example #{example_index}/#{examples.length()}: loss #{loss.tensor().get(0, 0)}")
    end
    example_index += 1
  end
  mean_loss = total_loss / examples.length()
  puts("epoch #{epoch} done: mean loss #{mean_loss}")
  Checkpoint.save(model, checkpoint_path)
  epoch += 1
end

puts("training complete, final checkpoint at #{checkpoint_path}")
