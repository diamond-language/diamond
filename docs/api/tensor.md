# Tensor

[API reference](index.md) · Previous: [Crypto and encoding](crypto.md)

A `Tensor` is a dense two-dimensional matrix of Floats stored in native memory,
for numeric work that would be slow element by element. There is no
broadcasting, no other number of dimensions and no automatic differentiation;
`examples/transformer` builds a small autograd on top of these methods.

```ruby
a = Tensor.from_array([[1, 2], [3, 4]])
a.matmul(Tensor.from_array([[1], [1]])).to_a()   # => [[3.0], [7.0]]
a.transpose().to_a()                             # => [[1.0, 3.0], [2.0, 4.0]]
```

## Creating

### `Tensor.zeros(rows, cols) -> Tensor`

A matrix of zeros.

### `Tensor.from_array(rows) -> Tensor`

A matrix from an Array of equal-length Arrays of numbers.

### `Tensor.random(rows, cols, seed) -> Tensor`

Values in `[-1, 1)` from a deterministic generator seeded with `seed`.

## Reading and writing

### `rows() -> Int`

### `cols() -> Int`

The shape.

### `get(row, col) -> Float`

### `set(row, col, value)`

One element. An index outside the matrix raises `IndexError`.

### `to_a() -> Array`

The values as nested Arrays.

### `clone() -> Tensor`

A copy.

## Arithmetic

### `matmul(other) -> Tensor`

The matrix product, computed on several threads. Mismatched shapes raise
`TypeError`.

### `transpose() -> Tensor`

A transposed copy.

### `column_sums() -> Tensor`

A one-row matrix of each column's sum.

### `columns(start, count) -> Tensor`

A copy of `count` columns starting at `start`.

## In-place operations

These change the receiver and return it.

### `add!(other) -> Tensor`

Adds a matrix of the same shape.

### `scale!(factor) -> Tensor`

Multiplies every element.

### `add_bias!(bias) -> Tensor`

Adds a one-row matrix to every row.

### `add_columns!(start, other) -> Tensor`

Adds `other` to the columns starting at `start`.

### `row_softmax!() -> Tensor`

Replaces each row with its softmax.

### `gelu!() -> Tensor`

Applies GELU to every element.

```ruby
Tensor.from_array([[1, 2]]).add_bias!(Tensor.from_array([[10, 20]])).to_a()   # => [[11.0, 22.0]]
```

## Gradients

### `softmax_backward(grad_output) -> Tensor`

### `gelu_backward(grad_output) -> Tensor`

The gradient with respect to the input, called on the original input.

### `layernorm_forward(gamma, beta, eps) -> Array`

Layer normalization of each row: `[output, normalized, inv_std]`.

### `layernorm_backward(gamma, grad_output, inv_std) -> Array`

Called on `layernorm_forward`'s `normalized`: `[grad_x, grad_gamma, grad_beta]`.
