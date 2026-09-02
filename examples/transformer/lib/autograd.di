# Reverse-mode autodiff ops. Every op here is a `self.` class method
# (not a plain top-level `def`) purely because Diamond's `closure`
# needs an enclosing method with a bound `self` to capture into --
# confirmed directly, a bare top-level `def` can't declare one. Each
# op computes its forward value via the native/tensor_ops Tensor
# calls, wraps the result in a Var recording its parents, and attaches
# a `backward` closure that -- given the result Var's own already-
# accumulated `.grad` (guaranteed complete by the time it runs, since
# `backward!` below calls every node's backward_fn in strict reverse
# topological order) -- adds this op's contribution into each parent's
# `.grad`. Every backward formula here was checked against numerical
# (finite-difference) gradients before being wired into the model --
# see gradcheck.di.
class Autograd
  # Sum of every element, as a 1x1 Var -- the scalar `backward!` needs
  # to seed from. Its own backward simply broadcasts the (also scalar)
  # incoming gradient back onto every element it summed.
  def self.sum(x)
    total = 0.0
    i = 0
    while i < x.tensor().rows()
      j = 0
      while j < x.tensor().cols()
        total += x.tensor().get(i, j)
        j += 1
      end
      i += 1
    end
    output = Tensor.zeros(1, 1)
    output.set(0, 0, total)
    result = Var.new(output, [x], true)
    closure backward()
      if x.requires_grad()
        seed = result.grad().get(0, 0)
        i = 0
        while i < x.tensor().rows()
          j = 0
          while j < x.tensor().cols()
            x.grad().set(i, j, x.grad().get(i, j) + seed)
            j += 1
          end
          i += 1
        end
      end
    end
    result.backward_fn = backward
    result
  end

  def self.add(a, b)
    result = Var.new(tensor_add!(tensor_clone(a.tensor()), b.tensor()), [a, b], true)
    closure backward()
      if a.requires_grad()
        tensor_add!(a.grad(), result.grad())
      end
      if b.requires_grad()
        tensor_add!(b.grad(), result.grad())
      end
    end
    result.backward_fn = backward
    result
  end

  # x: (rows x cols). bias: (1 x cols), broadcast over every row.
  def self.add_bias(x, bias)
    result = Var.new(tensor_add_bias!(tensor_clone(x.tensor()), bias.tensor()), [x, bias], true)
    closure backward()
      if x.requires_grad()
        tensor_add!(x.grad(), result.grad())
      end
      if bias.requires_grad()
        # dL/dbias_j = sum over rows of dL/dy_ij -- the same broadcast
        # bias.tensor() got added to every row of x, so its own
        # gradient is the column-sum of the output gradient.
        tensor_add!(bias.grad(), tensor_column_sums(result.grad()))
      end
    end
    result.backward_fn = backward
    result
  end

  def self.matmul(a, b)
    result = Var.new(a.tensor().matmul(b.tensor()), [a, b], true)
    closure backward()
      if a.requires_grad()
        # dL/dA = dL/dC . B^T
        tensor_add!(a.grad(), result.grad().matmul(b.tensor().transpose()))
      end
      if b.requires_grad()
        # dL/dB = A^T . dL/dC
        tensor_add!(b.grad(), a.tensor().transpose().matmul(result.grad()))
      end
    end
    result.backward_fn = backward
    result
  end

  # x scaled by a plain (non-Var) Float scalar -- nothing here is ever
  # differentiated *with respect to* the scalar itself (every scalar
  # this model uses, like attention's 1/sqrt(head_dim), is a fixed
  # architectural constant, not a learned parameter).
  def self.scale(x, scalar)
    result = Var.new(tensor_scale!(tensor_clone(x.tensor()), scalar), [x], true)
    closure backward()
      if x.requires_grad()
        tensor_add!(x.grad(), tensor_scale!(tensor_clone(result.grad()), scalar))
      end
    end
    result.backward_fn = backward
    result
  end

  def self.transpose(x)
    result = Var.new(x.tensor().transpose(), [x], true)
    closure backward()
      if x.requires_grad()
        tensor_add!(x.grad(), result.grad().transpose())
      end
    end
    result.backward_fn = backward
    result
  end

  def self.columns(x, start, width)
    result = Var.new(tensor_columns(x.tensor(), start, width), [x], true)
    closure backward()
      if x.requires_grad()
        tensor_add_columns!(x.grad(), start, result.grad())
      end
    end
    result.backward_fn = backward
    result
  end

  def self.concat_columns(vars)
    tensors = []
    i = 0
    while i < vars.length()
      tensors.push(vars[i].tensor())
      i += 1
    end
    result = Var.new(tensor_concat_columns(tensors), vars, true)
    closure backward()
      col_offset = 0
      i = 0
      while i < vars.length()
        v = vars[i]
        width = v.tensor().cols()
        if v.requires_grad()
          tensor_add_columns!(v.grad(), 0, tensor_columns(result.grad(), col_offset, width))
        end
        col_offset += width
        i += 1
      end
    end
    result.backward_fn = backward
    result
  end

  # Native forward (Tensor#layernorm_forward) + native backward
  # (Tensor#layernorm_backward) -- the standard LayerNorm formula
  # (both directions), moved to C after measuring that the plain
  # Diamond-loop version of this (still visible in this file's own
  # history) was as much of a real training step's cost as its
  # forward-mutator counterparts (add!/scale!/etc) already fixed
  # turned out to be, once measured directly rather than assumed.
  # `normalized`/`inv_std` are exactly the two things forward computes
  # that backward also needs -- caching them here (closed over by
  # `backward`) avoids recomputing mean/variance a second time.
  def self.layernorm(x, gamma, beta, eps)
    forward_result = x.tensor().layernorm_forward(gamma.tensor(), beta.tensor(), eps)
    output = forward_result[0]
    normalized = forward_result[1]
    inv_std = forward_result[2]
    result = Var.new(output, [x, gamma, beta], true)
    closure backward()
      if x.requires_grad() || gamma.requires_grad() || beta.requires_grad()
        backward_result = normalized.layernorm_backward(gamma.tensor(), result.grad(), inv_std)
        if x.requires_grad()
          tensor_add!(x.grad(), backward_result[0])
        end
        if gamma.requires_grad()
          tensor_add!(gamma.grad(), backward_result[1])
        end
        if beta.requires_grad()
          tensor_add!(beta.grad(), backward_result[2])
        end
      end
    end
    result.backward_fn = backward
    result
  end

  # Row-wise softmax -- standalone (not fused with a loss), used inside
  # attention on raw scores. See loss.di's own softmax_cross_entropy
  # for the *fused* version used at the actual training loss, whose
  # gradient simplifies to probs-minus-one-hot instead of needing this.
  # Native forward (#row_softmax!) + native backward
  # (#softmax_backward, which only needs the softmax's own output, not
  # the pre-softmax input -- see its own comment in src/vm.c).
  def self.softmax(x)
    output = tensor_clone(x.tensor())
    tensor_row_softmax!(output)
    result = Var.new(output, [x], true)
    closure backward()
      if x.requires_grad()
        tensor_add!(x.grad(), output.softmax_backward(result.grad()))
      end
    end
    result.backward_fn = backward
    result
  end

  # Native forward (#gelu!) + native backward (#gelu_backward, which
  # needs GELU's original *input* -- x.tensor(), unmutated since
  # #gelu! only ever ran on the cloned `output` -- not its output).
  def self.gelu(x)
    output = tensor_clone(x.tensor())
    tensor_gelu!(output)
    result = Var.new(output, [x], true)
    closure backward()
      if x.requires_grad()
        tensor_add!(x.grad(), x.tensor().gelu_backward(result.grad()))
      end
    end
    result.backward_fn = backward
    result
  end

  # token_table/position_table: Vars wrapping (vocab_size x d_model) /
  # (max_seq_len x d_model) tables. token_ids: plain Array of Int.
  # Gradient scatters back into the specific rows that were looked up
  # (summed if a row is looked up more than once -- a repeated token in
  # the same sequence, most obviously).
  def self.embedding(token_table, position_table, token_ids)
    d_model = token_table.tensor().cols()
    seq_len = token_ids.length()
    output = Tensor.zeros(seq_len, d_model)
    i = 0
    while i < seq_len
      token_id = token_ids[i]
      j = 0
      while j < d_model
        output.set(i, j, token_table.tensor().get(token_id, j) + position_table.tensor().get(i, j))
        j += 1
      end
      i += 1
    end
    result = Var.new(output, [token_table, position_table], true)
    closure backward()
      i = 0
      while i < seq_len
        token_id = token_ids[i]
        j = 0
        while j < d_model
          dy = result.grad().get(i, j)
          if token_table.requires_grad()
            token_table.grad().set(token_id, j, token_table.grad().get(token_id, j) + dy)
          end
          if position_table.requires_grad()
            position_table.grad().set(i, j, position_table.grad().get(i, j) + dy)
          end
          j += 1
        end
        i += 1
      end
    end
    result.backward_fn = backward
    result
  end
end

def topo_visit(node, visited, order)
  if visited.include?(node)
    return
  end
  visited.push(node)
  i = 0
  while i < node.parents().length()
    topo_visit(node.parents()[i], visited, order)
    i += 1
  end
  order.push(node)
end

# Runs backward from `loss_var` (must be a 1x1 Var -- the scalar loss)
# through the whole graph it was built from, accumulating into every
# reachable Var's own `.grad`. Does NOT zero any gradients first --
# call `.zero_grad!()` on every trainable leaf before the forward pass
# if you don't want this step's gradients added on top of a previous
# step's (optimizer.di's own training loop does this).
def backward!(loss_var)
  order = []
  visited = []
  topo_visit(loss_var, visited, order)
  loss_var.grad().set(0, 0, 1.0)
  i = order.length() - 1
  while i >= 0
    node = order[i]
    fn = node.backward_fn()
    if fn != nil
      fn()
    end
    i -= 1
  end
end
