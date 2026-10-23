# The backward and layernorm Tensor operations reject mismatched shapes instead of reading past a Tensor.
def mismatch(index, big, small, one)
  begin
    if index == 0 then big.softmax_backward(small)
    elsif index == 1 then big.gelu_backward(small)
    elsif index == 2 then big.layernorm_forward(one, one, 0.00001)
    elsif index == 3 then big.layernorm_backward(one, small, one)
    else big.layernorm_backward(one, big, small)
    end
    "accepted"
  rescue e: TypeError
    "rejected"
  end
end

big = Tensor.zeros(4, 4)
small = Tensor.zeros(1, 1)
one = Tensor.zeros(1, 1)
results = []
index = 0
while index < 5
  results.push(mismatch(index, big, small, one))
  index += 1
end
ok = big.softmax_backward(Tensor.zeros(4, 4)).rows()
parts = big.layernorm_forward(Tensor.zeros(1, 4), Tensor.zeros(1, 4), 0.00001)
results.push(ok)
results.push(parts.length())
results
