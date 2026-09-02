# Elementwise/shape Tensor helpers built entirely on top of the native
# #get/#set/#rows/#cols/#matmul/#transpose (packages/tensor is out of
# scope for this scaffold -- these live here, not in the language core,
# same reasoning as this whole example's own top-level README). None of
# these are O(n^3) like #matmul itself, so a plain per-element Diamond
# loop is fine -- no need for more native C surface here.

# A fresh, independent copy -- needed anywhere an in-place mutator
# (tensor_add!, tensor_scale!, tensor_gelu!, tensor_row_softmax!, ...)
# is about to be used to compute a *new* forward value from an
# existing Var's .tensor(): the original must survive unmutated, both
# because other ops may still read it (a Var can be used as the parent
# of more than one op) and because several backward passes (gelu,
# layernorm, softmax) read the *original* pre-op values, not the
# post-op ones.
def tensor_clone(x)
  result = Tensor.zeros(x.rows(), x.cols())
  i = 0
  while i < x.rows()
    j = 0
    while j < x.cols()
      result.set(i, j, x.get(i, j))
      j += 1
    end
    i += 1
  end
  result
end

# Sums each column across every row -- a (1 x cols) Tensor. The
# backward-pass shape for a bias that was broadcast-added to every row
# in the forward pass (Autograd.add_bias).
def tensor_column_sums(x)
  result = Tensor.zeros(1, x.cols())
  i = 0
  while i < x.rows()
    j = 0
    while j < x.cols()
      result.set(0, j, result.get(0, j) + x.get(i, j))
      j += 1
    end
    i += 1
  end
  result
end

# Adds src into dest's columns [start, start+src.cols()), in place --
# the backward-pass shape for tensor_columns's own forward slice
# (Autograd.columns/#concat_columns): a gradient contribution into a
# specific column range of a wider Tensor, accumulated (not
# overwritten) since that range may receive contributions from more
# than one op.
def tensor_add_columns!(dest, start, src)
  i = 0
  while i < src.rows()
    j = 0
    while j < src.cols()
      dest.set(i, start + j, dest.get(i, start + j) + src.get(i, j))
      j += 1
    end
    i += 1
  end
  dest
end

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
# [-1, 1) -- scaling is a cheap O(rows*cols) elementwise pass, nowhere
# near the cost the native random-fill saves (see rng.di's own comment
# on why this doesn't build via Tensor.from_array/a Diamond-level
# nested Array any more).
def tensor_random(rows, cols, rng, scale)
  tensor = Tensor.random(rows, cols, rng.next_seed())
  tensor_scale!(tensor, scale)
  tensor
end

# a += b, in place. Both must be the same shape.
def tensor_add!(a, b)
  i = 0
  while i < a.rows()
    j = 0
    while j < a.cols()
      a.set(i, j, a.get(i, j) + b.get(i, j))
      j += 1
    end
    i += 1
  end
  a
end

# Adds a single-row bias Tensor (1 x cols) to every row of x, in place.
def tensor_add_bias!(x, bias)
  i = 0
  while i < x.rows()
    j = 0
    while j < x.cols()
      x.set(i, j, x.get(i, j) + bias.get(0, j))
      j += 1
    end
    i += 1
  end
  x
end

def tensor_scale!(x, scalar)
  i = 0
  while i < x.rows()
    j = 0
    while j < x.cols()
      x.set(i, j, x.get(i, j) * scalar)
      j += 1
    end
    i += 1
  end
  x
end

# In-place softmax over each row -- the max-subtraction step is the
# standard numerical-stability trick (keeps exp()'s argument <= 0, so
# it can't overflow no matter how large the raw scores get).
def tensor_row_softmax!(x)
  i = 0
  while i < x.rows()
    max = x.get(i, 0)
    j = 1
    while j < x.cols()
      value = x.get(i, j)
      if value > max
        max = value
      end
      j += 1
    end
    sum = 0.0
    j = 0
    while j < x.cols()
      weight = exp(x.get(i, j) - max)
      x.set(i, j, weight)
      sum += weight
      j += 1
    end
    j = 0
    while j < x.cols()
      x.set(i, j, x.get(i, j) / sum)
      j += 1
    end
    i += 1
  end
  x
end

# Row-wise LayerNorm: (x - mean) / sqrt(var + eps) * gamma + beta.
# gamma/beta are 1 x cols Tensors (learned scale/shift -- untrained
# here, just initialized to 1s/0s, the identity transform). Returns a
# fresh Tensor rather than mutating in place: the pre-normalization
# residual stream (x itself) still needs to survive for the residual
# add after attention/feed-forward.
def tensor_layernorm(x, gamma, beta, eps)
  result = Tensor.zeros(x.rows(), x.cols())
  i = 0
  while i < x.rows()
    sum = 0.0
    j = 0
    while j < x.cols()
      sum += x.get(i, j)
      j += 1
    end
    mean = sum / x.cols()
    variance_sum = 0.0
    j = 0
    while j < x.cols()
      diff = x.get(i, j) - mean
      variance_sum += diff * diff
      j += 1
    end
    variance = variance_sum / x.cols()
    denominator = sqrt(variance + eps)
    j = 0
    while j < x.cols()
      normalized = (x.get(i, j) - mean) / denominator
      result.set(i, j, normalized * gamma.get(0, j) + beta.get(0, j))
      j += 1
    end
    i += 1
  end
  result
end

# Columns [start, start+width) of x, as a fresh Tensor -- splits a QKV
# projection's d_model columns into one attention head's own slice.
def tensor_columns(x, start, width)
  result = Tensor.zeros(x.rows(), width)
  i = 0
  while i < x.rows()
    j = 0
    while j < width
      result.set(i, j, x.get(i, start + j))
      j += 1
    end
    i += 1
  end
  result
end

# Concatenates same-row-count Tensors side by side (columns) --
# reassembles per-head attention outputs back into one d_model-wide
# Tensor.
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
    i = 0
    while i < rows
      j = 0
      while j < tensor.cols()
        result.set(i, col_offset + j, tensor.get(i, j))
        j += 1
      end
      i += 1
    end
    col_offset += tensor.cols()
    t += 1
  end
  result
end

# Numerically stable scalar tanh -- Diamond's math builtins (src/vm.h's
# DiamondMathFunction) don't include tanh, only sqrt/sin/cos/tan/pow/
# exp/log, so this is built from exp() rather than adding another VM
# opcode for one activation function. Branches on sign to keep exp()'s
# argument always <= 0 (exp(2x) would overflow for large positive x;
# exp(-2x) can't, and the two branches are algebraically equivalent).
def scalar_tanh(x)
  if x >= 0
    e = exp(0.0 - 2.0 * x)
    (1.0 - e) / (1.0 + e)
  else
    e = exp(2.0 * x)
    (e - 1.0) / (e + 1.0)
  end
end

# GELU (tanh approximation -- the common transformer choice, e.g.
# GPT-2's own), applied elementwise, in place.
def tensor_gelu!(x)
  i = 0
  while i < x.rows()
    j = 0
    while j < x.cols()
      value = x.get(i, j)
      inner = 0.7978845608028654 * (value + 0.044715 * value * value * value)
      x.set(i, j, 0.5 * value * (1.0 + scalar_tanh(inner)))
      j += 1
    end
    i += 1
  end
  x
end
