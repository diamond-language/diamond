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
- `lib/tensor_ops.di` -- elementwise/shape helpers built on the native
  `#get`/`#set`/`#matmul`/`#transpose`: add, add-bias, scale, row
  softmax, LayerNorm, column slicing/concatenation (for splitting a
  projection into per-head slices and reassembling them), GELU. None
  of these are O(n^3) like `#matmul` itself, so a plain per-element
  Diamond loop is fine for all of them -- no native C needed here.
- `lib/linear.di`, `lib/attention.di` (multi-head self-attention, with
  causal masking), `lib/feed_forward.di`, `lib/transformer_block.di`
  (pre-norm: LN -> attention -> residual, LN -> feed-forward ->
  residual), `lib/embedding.di` (token + learned positional), and
  `lib/model.di` (stacks it all: embedding -> N blocks -> final LN ->
  vocab projection).
- `demo.di` -- builds a tiny (vocab=50, d_model=32, 4 heads, 2 layers)
  randomly-initialized model and runs one real forward pass end to
  end, printing the output shape and an (untrained, meaningless)
  argmax next-token prediction.
- `benchmark.di` -- the same thing at a GPT-2-small-ish scale
  (vocab=8000, d_model=512, 8 heads, d_ff=2048, 6 layers, seq_len=64),
  for a real build-time/forward-pass-time number instead of a toy one.

Run either with `../../build/diamond demo.di` (or `benchmark.di`) from
this directory.

## Scope

**Forward pass (inference) only. No training.** There is no
backpropagation, no autodiff, no optimizer, no loss function anywhere
in this scaffold -- every weight is randomly initialized and stays
that way. Building a real training loop (gradients through every one
of these ops, an optimizer, a data pipeline) is a substantially larger
project than this scaffold and a deliberate next decision, not
something implied by what's here.

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

**No tokenizer.** `demo.di`/`benchmark.di` both feed in already-integer
token ids (`Array` of `Int`) directly.
