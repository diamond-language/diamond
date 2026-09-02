# Transformer (scaffold, branch `tensor-experiment`)

A from-scratch, GPT-style decoder-only transformer built on this
branch's native `Tensor` (`DIAMOND_OBJECT_TENSOR`, `src/vm.c`/`vm.h`/
`object.h`/`compiler.c`) -- motivated by an earlier Ruby attempt
(native extensions included) that was still too slow. Diamond's own
`Array` is boxed `DiamondValue*` with no packed float buffer and no
SIMD anywhere in the VM, so a matmul in pure Diamond loops sits at
roughly the same performance class Ruby did; `Tensor#matmul` is a
real, threaded, k-blocked native implementation instead (38-62 GFLOPS
on BERT-base-shaped matmuls, see this branch's own commit history for
the before/after numbers).

Deliberately **not on `main`** -- this is still an experiment, kept on
its own branch so it doesn't need to be a fully-committed-to language
feature yet (see the branch's own root commit message).

## What's here

- `lib/rng.di` -- a deterministic LCG, only ever used to seed
  `Tensor.random` (the actual random fill happens natively in C, not
  in a Diamond loop -- see its own comment for why that distinction
  mattered).
- `lib/tensor_ops.di` -- thin Diamond wrappers around native Tensor
  methods: add!/scale!/add_bias!/row_softmax!/gelu!/columns/
  concat_columns/column_sums/clone, plus LayerNorm's forward+backward
  pair (`#layernorm_forward`/`#layernorm_backward`) and softmax/GELU's
  own backward passes (`#softmax_backward`/`#gelu_backward`), all in
  `src/vm.c`. These were *not* native in this scaffold's first pass
  ("none of these are O(n^3) like #matmul, a plain Diamond loop is
  fine" -- true for correctness, wrong for speed): one training step
  measured 5.55s before moving the forward mutators to C, barely
  improved (7.24s -- noise) until the *backward* passes were moved too,
  landing at 4.19s -- see autograd.di's own history for the full
  finding, including why the improvement was more modest than hoped
  (the remaining cost is Var/Autograd's own per-op bookkeeping --
  allocation, closures, GC -- not element-wise math anymore).
- `lib/var.di` -- `Var`: one node in the reverse-mode autodiff graph
  (a Tensor, its accumulated gradient, its parent Vars, and a
  `backward` closure).
- `lib/autograd.di` -- `Autograd.*`: every differentiable op (sum, add,
  add_bias, matmul, scale, transpose, columns/concat_columns,
  layernorm, softmax, gelu, embedding), each building a `Var` and
  recording how to push gradient back into its own parents; `backward!`
  walks the graph in reverse topological order. Every formula here was
  checked against numerical (finite-difference) gradients -- see
  `gradcheck.di` -- before being trusted in the actual model.
- `lib/loss.di` -- `Loss.softmax_cross_entropy`: the standard
  next-token-prediction training objective, fused (not composed from
  `Autograd.softmax` + a separate log/NLL op) for numerical stability
  and a simpler gradient.
- `lib/optimizer.di` -- `SGD`: plain gradient descent, no momentum/Adam
  (enough to prove the graph produces a useful training signal, which
  is what `train.di` is actually checking for).
- `lib/linear.di`, `lib/attention.di` (multi-head self-attention, with
  causal masking), `lib/feed_forward.di`, `lib/transformer_block.di`
  (pre-norm: LN -> attention -> residual, LN -> feed-forward ->
  residual), `lib/embedding.di` (token + learned positional), and
  `lib/model.di` (stacks it all: embedding -> N blocks -> final LN ->
  vocab projection; `#parameters` collects every trainable leaf `Var`
  for an optimizer to walk).
- `demo.di` -- builds a tiny (vocab=50, d_model=32, 4 heads, 2 layers)
  randomly-initialized model and runs one real forward pass end to
  end, printing the output shape and an (untrained, meaningless)
  argmax next-token prediction. Never calls `backward!` -- the
  autograd graph `#forward` builds is simply unused here.
- `benchmark.di` -- the same thing at a GPT-2-small-ish scale
  (vocab=8000, d_model=512, 8 heads, d_ff=2048, 6 layers, seq_len=64),
  for a real build-time/forward-pass-time number instead of a toy one.
- `gradcheck.di` -- numerically verifies every `Autograd.*` backward
  formula. Run this after touching `autograd.di`.
- `train.di` -- trains a tiny model on a trivial fixed toy task
  ("next id = id + 1" over a short sequence) for 200 SGD steps,
  printing loss every 20. Loss goes from ~2.9 to ~0.02 and the model
  gets every prediction right by the end -- the actual proof training
  works end to end, not just that it's wired up.
- `lib/tokenizer.di` -- `ByteTokenizer`: byte-level (vocab_size always
  256, every byte value is its own token), no vocabulary-building pass
  needed, works on any file unchanged including non-ASCII text.
- `lib/corpus.di` -- `Corpus.load(folder, extensions, max_files,
  max_stories)`: assembles training text from every matching file
  under a folder, walked recursively via `Dir.entries`/`File.
  directory?` (main branch, this session -- an earlier version of this
  shelled out to `find` via `Process.run`, since Diamond had neither
  primitive at the time), sorted byte-lexicographically so "the first
  N files" means what it sounds like on a numbered dataset like
  TinyStories' `data00.json`..`data50.json` (only correct for
  zero-padded numbering -- see the function's own comment).
  `.txt`/`.md` contribute their raw content; `.json` is parsed and
  walked for TinyStories' own shape (`[{"story": "...", ...}, ...]`,
  or a plain array of strings) -- and parsing is genuinely slow at
  real scale (~330ms/MB measured directly against the actual
  TinyStories_all_data corpus, ~140MB/shard means ~45s just to parse
  one file), which is what `max_files`/`max_stories` are for: start
  small while iterating, raise them once you're ready to commit the
  time.
- `lib/dataset.di` -- `Dataset.windows(token_ids, seq_len, stride)`:
  chops a whole tokenized corpus into fixed-length (input, target)
  training examples (the model has no batching dimension, so this is
  what "one training example" means against a real corpus rather than
  a hand-written toy list).
- `lib/checkpoint.di` -- `Checkpoint.save`/`.load!`: a trained model's
  weights to/from a plain JSON file (every parameter Tensor, in
  `model.parameters()`'s own fixed order, as nested arrays via
  `Tensor#to_a`/`.from_array`) -- round-tripped and verified directly.
- `train_corpus.di` -- the practical trainer: `Corpus.load` a real
  folder, `ByteTokenizer.encode`, `Dataset.windows`, one SGD step per
  window, `Checkpoint.save` after every epoch. Prints an estimated
  total training time up front (based on a measured seconds/step
  constant) before committing to a run -- adjust `max_stories`/
  `epochs` if that number is too long. Usage: `../../build/diamond
  train_corpus.di <folder> [checkpoint_path] [max_files] [max_stories]`.
- `generate.di` -- loads a checkpoint and greedily (argmax, no
  temperature/top-k yet) generates text continuing a prompt, one byte
  at a time. No K/V cache -- every step re-runs the full forward pass
  over the whole context so far (clamped to the last `max_seq_len`
  tokens, since the positional embedding table only has that many
  rows), fine for a demo, not how a real implementation would do this.
  Usage: `../../build/diamond generate.di <checkpoint_path> <prompt>
  [num_tokens]`. Its own model config (d_model/heads/d_ff/layers/
  seq_len) must exactly match whatever `train_corpus.di` was run
  with -- `Checkpoint.load!` only checks the parameter *count*, not
  each Tensor's shape.

Run any of them with `../../build/diamond demo.di` (or `benchmark.di`,
`gradcheck.di`, `train.di`, `train_corpus.di`, `generate.di`) from this
directory.

## Scope

**Training exists now (`autograd.di`/`loss.di`/`optimizer.di`/
`train.di`) -- plain SGD, no momentum/Adam, no learning-rate schedule,
no batching (one sequence at a time), no data pipeline (`train.di`'s
"dataset" is a single hardcoded toy sequence).** `demo.di`/
`benchmark.di` still only exercise the forward pass (they never call
`backward!`), so their own numbers don't reflect training-loop cost --
building the autograd graph on every forward call (needed for
`train.di`) adds real overhead over the original forward-only version
of this scaffold even when nothing ever reads it, which `demo.di`/
`benchmark.di` don't currently avoid (no "no_grad" inference mode
exists yet to skip graph construction).

**`Tensor` is 2D-only.** No reshape, no batching dimension, no
broadcasting. Multi-head attention works around this by column-slicing
the (seq_len x d_model) projections into per-head (seq_len x head_dim)
Tensors and concatenating the per-head outputs back together (both in
`tensor_ops.di`) -- correct, but real ML frameworks would do this as a
zero-copy reshape/view instead of an actual O(seq_len*d_model) copy
each way. Not the bottleneck (matmul's O(n^3) dominates), but worth
knowing if `Tensor` ever grows a real reshape/view story.

**Weight init is a loose approximation**, not a rigorous Xavier/He/etc.
implementation -- there's no training to actually benefit from a
carefully-tuned init distribution, so it only needs to avoid a
degenerate all-zero or NaN-producing forward pass, which it does.

**`demo.di`/`benchmark.di` still feed in already-integer token ids
directly** (not through `ByteTokenizer`) -- they predate the
tokenizer/corpus work and were never updated, since their whole point
is exercising the model shape/timing directly, not the text pipeline
around it.

**Training throughput is real but modest, not fast.** ~4.2s/step
measured at this scaffold's actual training config (d_model=128, 4
layers, seq_len=64) after moving every hot elementwise op (forward
*and* backward) to native C -- meaningfully better than the ~5.55s/step
this measured before that work, but nowhere near what the native-op
speedup alone might suggest, because the remaining cost is Var/
Autograd's own per-op overhead (a fresh Tensor/Var/closure allocated
per op, GC pressure), not element-wise math. At that rate, one epoch
over even a modest few-hundred-story slice of a real corpus is
measured in hours, and the full TinyStories corpus (51 shards, ~7GB,
~2.6M stories) is not remotely tractable without either a much smaller
model/corpus or further work on the per-op overhead itself -- a
different, harder problem than "which loop should be in C" (batching
multiple ops together, avoiding the zero-initialized grad allocation
until a gradient is actually needed, etc.), not yet attempted here.
