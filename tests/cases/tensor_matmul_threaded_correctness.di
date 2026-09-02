# Cross-validates the C-level threaded+k-blocked Tensor#matmul against
# an independent, naive Diamond-level triple loop, at a size chosen to
# exceed DIAMOND_TENSOR_MATMUL_THREAD_FLOOR (2*m*k*n >= 1MB) and to
# span multiple k-blocks -- so this actually exercises the row-range
# splitting and block_k reordering, not just the trivial single-
# threaded fallback a small matrix would take.
def random_matrix(rows, cols, seed)
  data = []
  i = 0
  while i < rows
    row = []
    j = 0
    while j < cols
      row.push(((i * 131 + j * 977 + seed * 7919) % 1000) / 100.0 - 5.0)
      j += 1
    end
    data.push(row)
    i += 1
  end
  data
end

def naive_matmul(a_data, b_data, m, k, n)
  result = []
  i = 0
  while i < m
    row = []
    j = 0
    while j < n
      sum = 0.0
      p = 0
      while p < k
        sum += a_data[i][p] * b_data[p][j]
        p += 1
      end
      row.push(sum)
      j += 1
    end
    result.push(row)
    i += 1
  end
  result
end

def max_abs_diff(a, b, m, n)
  worst = 0.0
  i = 0
  while i < m
    j = 0
    while j < n
      raw_diff = a[i][j] - b[i][j]
      diff = if raw_diff < 0 then 0 - raw_diff else raw_diff end
      if diff > worst then worst = diff end
      j += 1
    end
    i += 1
  end
  worst
end

m = 130
k = 300
n = 140

a_data = random_matrix(m, k, 1)
b_data = random_matrix(k, n, 2)

c = Tensor.from_array(a_data).matmul(Tensor.from_array(b_data))
expected = naive_matmul(a_data, b_data, m, k, n)
diff = max_abs_diff(c.to_a(), expected, m, n)

[c.rows(), c.cols(), diff < 0.0000001]
