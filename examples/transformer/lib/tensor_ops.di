# Elementwise/shape Tensor helpers -- thin Diamond-level wrappers
# around the native Tensor mutators (add!/scale!/add_bias!/
# row_softmax!/gelu!/column_sums/columns/add_columns!/clone, src/vm.c)
# rather than doing their own per-element #get/#set loops, which used
# to be the actual bottleneck at real training scale: one training
# step at examples/transformer's real config (d_model=128, 4 layers,
# seq_len=64) measured 5.55s before this change, almost entirely spent
# in these functions' own interpreted loops (thousands of individual
# native-dispatch #get/#set round-trips per call), not in #matmul
# (mostly too small at this scale to even cross
# DIAMOND_TENSOR_MATMUL_THREAD_FLOOR). Every function here keeps its
# exact original name/signature/semantics so autograd.di needed zero
# changes -- only the underlying mechanics got faster. LayerNorm's
# forward+backward pair (also originally a plain Diamond loop here)
# turned out to matter just as much and is now native too
# (Tensor#layernorm_forward/#layernorm_backward) -- Autograd.layernorm
# (autograd.di) calls those two directly rather than through a wrapper
# in this file, since its own backward closure needs the `normalized`/
# `inv_std` values forward returns, not just a final output Tensor.

def tensor_clone(x) = x.clone()

def tensor_column_sums(x) = x.column_sums()

def tensor_add_columns!(dest, start, src) = dest.add_columns!(start, src)

def tensor_ones(rows, cols)
  result = Tensor.zeros(rows, cols)
  i = 0
  while i < rows
    j = 0
    while j < cols
      result.set(i, j, 1.0)
      j += 1
    end
    i += 1
  end
  result
end

# Values uniform in [-scale, scale). Tensor.random itself only produces
# [-1, 1) -- scaling is a cheap O(rows*cols) native pass, nowhere near
# the cost the native random-fill saves (see rng.di's own comment on
# why this doesn't build via Tensor.from_array/a Diamond-level nested
# Array any more).
def tensor_random(rows, cols, rng, scale)
  tensor = Tensor.random(rows, cols, rng.next_seed())
  tensor_scale!(tensor, scale)
  tensor
end

# a += b, in place. Both must be the same shape.
def tensor_add!(a, b) = a.add!(b)

# Adds a single-row bias Tensor (1 x cols) to every row of x, in place.
def tensor_add_bias!(x, bias) = x.add_bias!(bias)

def tensor_scale!(x, scalar) = x.scale!(scalar)

# In-place softmax over each row.
def tensor_row_softmax!(x) = x.row_softmax!()

# GELU (tanh approximation -- the common transformer choice, e.g.
# GPT-2's own), applied elementwise, in place.
def tensor_gelu!(x) = x.gelu!()

# Columns [start, start+width) of x, as a fresh Tensor -- splits a QKV
# projection's d_model columns into one attention head's own slice.
def tensor_columns(x, start, width) = x.columns(start, width)

# Concatenates same-row-count Tensors side by side (columns) --
# reassembles per-head attention outputs back into one d_model-wide
# Tensor. Built from #add_columns! rather than its own per-element
# loop: accumulating into an already-zeroed result is exactly a set,
# for disjoint column ranges like these.
def tensor_concat_columns(tensors)
  rows = tensors[0].rows()
  total_cols = 0
  t = 0
  while t < tensors.length()
    total_cols += tensors[t].cols()
    t += 1
  end
  result = Tensor.zeros(rows, total_cols)
  col_offset = 0
  t = 0
  while t < tensors.length()
    tensor = tensors[t]
    result.add_columns!(col_offset, tensor)
    col_offset += tensor.cols()
    t += 1
  end
  result
end
