# A plain LCG (linear congruential generator) -- deterministic (same
# seed -> same weights, same forward pass, reproducible across runs)
# and dependency-free, which is all weight initialization here actually
# needs. Not cryptographic and not a real ML framework's init strategy
# (Xavier/He/etc. get approximated loosely by Linear.random's own
# 1/sqrt(in_features) scale, not implemented rigorously here) -- there
# is no training in this scaffold, so init quality only matters enough
# to avoid a degenerate all-zero or NaN-producing forward pass.
#
# Only ever produces *seeds* now, not the random values themselves --
# tensor_random (tensor_ops.di) feeds each seed straight into the
# native Tensor.random, which fills the whole buffer in C. An earlier
# version of this class produced the values directly (one Diamond-level
# #sample() call per element via a slow Array-of-Arrays round trip
# through Tensor.from_array) -- fine for the small demo.di model, but
# it dominated build time at any real size (a vocab_size x d_model
# embedding table alone is easily millions of elements): 32s to build a
# 6-layer/512-dim/8000-vocab model, almost none of it actual compute.
class SimpleRng
  attr_accessor state

  def initialize(seed)
    @state = seed
  end

  # Advances the LCG and returns the new state as a seed for
  # Tensor.random -- every call returns a different seed, so distinct
  # weight matrices don't all end up with identical values. Named
  # `next_seed`, not `next` -- `next` is a reserved loop-control
  # keyword in Diamond.
  def next_seed()
    @state = mod(@state * 1103515245 + 12345, 2147483648)
    @state
  end
end
