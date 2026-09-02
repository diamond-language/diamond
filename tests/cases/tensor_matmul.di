a = Tensor.from_array([[1, 2], [3, 4]])
b = Tensor.from_array([[5, 6], [7, 8]])
c = a.matmul(b)

zeros = Tensor.zeros(2, 3)
zeros.set(0, 0, 1.5)
zeros.set(1, 2, 4.0)

shape_mismatch_rejected = false
begin
  Tensor.from_array([[1, 2]]).matmul(Tensor.from_array([[1, 2]]))
rescue error: TypeError
  shape_mismatch_rejected = true
end

bounds_rejected = false
begin
  Tensor.zeros(2, 2).get(5, 0)
rescue error: IndexError
  bounds_rejected = true
end

ragged_rejected = false
begin
  Tensor.from_array([[1, 2], [3]])
rescue error: TypeError
  ragged_rejected = true
end

[c.to_a(), c.rows(), c.cols(), zeros.to_a(), shape_mismatch_rejected, bounds_rejected, ragged_rejected]
