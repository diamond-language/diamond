t = Tensor.from_array([[1, 2, 3], [4, 5, 6]])
tt = t.transpose()

square = Tensor.from_array([[1, 2], [3, 4]])
double_transposed = square.transpose().transpose()

[tt.rows(), tt.cols(), tt.to_a(), double_transposed.to_a()]
