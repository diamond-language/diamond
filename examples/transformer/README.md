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

Run any of them with `../../build/diamond demo.di` (or `benchmark.di`,
`gradcheck.di`, `train.di`) from this directory.

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

**No tokenizer.** `demo.di`/`benchmark.di` both feed in already-integer
token ids (`Array` of `Int`) directly.
