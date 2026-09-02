# Numerically verifies every Autograd.* backward formula against
# finite differences before any of them are trusted inside the actual
# model -- perturbs each element of each input by +-epsilon, compares
# (f(x+eps)-f(x-eps))/(2*eps) against the analytic gradient backward!
# produced. Run this after touching autograd.di; a real bug here would
# otherwise show up only as "loss doesn't go down" in train.di, with
# no clue which op is wrong.
require "./lib/rng"
require "./lib/tensor_ops"
require "./lib/var"
require "./lib/autograd"

def random_tensor(rows, cols, seed)
  Tensor.random(rows, cols, seed)
end

# Runs `forward_fn` (a Callable taking no args, closing over whatever
# Vars it needs) to build a scalar-summed loss, calls backward!, then
# numerically perturbs every element of every Var in `inputs` and
# compares. `label` is just for the printed report.
def check(label, inputs, forward_fn)
  i = 0
  while i < inputs.length()
    inputs[i].zero_grad!()
    i += 1
  end
  loss = forward_fn()
  backward!(loss)

  # Snapshot every input's analytic gradient *before* any perturbation
  # -- var.grad() returns whatever Tensor is currently in that Var's
  # own @grad field, and zero_grad!() *replaces* that field (a fresh
  # Tensor.zeros(...), not an in-place reset) rather than mutating the
  # existing one in place. Reading var.grad() fresh, per input, deep
  # inside the loop below used to mean any input checked after the
  # first had its own analytic gradient silently already zeroed out by
  # then (found the hard way -- gradcheck itself was reporting the
  # multi-parent ops as broken, when the single shared root cause was
  # entirely here, not in any Autograd.* backward formula).
  analytics = []
  i = 0
  while i < inputs.length()
    analytics.push(tensor_clone(inputs[i].grad()))
    i += 1
  end

  epsilon = 0.0001
  max_diff = 0.0
  v = 0
  while v < inputs.length()
    var = inputs[v]
    analytic = analytics[v]
    row = 0
    while row < var.tensor().rows()
      col = 0
      while col < var.tensor().cols()
        original = var.tensor().get(row, col)

        # forward_fn() here only needs its *forward* value (plus/minus
        # below) -- no backward! call, so no gradient state to disturb
        # and nothing to zero.
        var.tensor().set(row, col, original + epsilon)
        plus = forward_fn()

        var.tensor().set(row, col, original - epsilon)
        minus = forward_fn()

        var.tensor().set(row, col, original)

        numeric = (plus.tensor().get(0, 0) - minus.tensor().get(0, 0)) / (2.0 * epsilon)
        computed = analytic.get(row, col)
        diff = computed - numeric
        if diff < 0
          diff = 0 - diff
        end
        if diff > max_diff
          max_diff = diff
        end
        col += 1
      end
      row += 1
    end
    v += 1
  end

  status = if max_diff < 0.001 then "PASS" else "FAIL" end
  puts("#{status} #{label}: max |analytic - numeric| = #{max_diff}")
end

class GradCheckRunner
  def self.run()
    a = Var.leaf(random_tensor(3, 4, 1))
    b = Var.leaf(random_tensor(3, 4, 2))
    closure check_add()
      Autograd.sum(Autograd.add(a, b))
    end
    check("add", [a, b], check_add)

    x = Var.leaf(random_tensor(3, 4, 3))
    bias = Var.leaf(random_tensor(1, 4, 4))
    closure check_add_bias()
      Autograd.sum(Autograd.add_bias(x, bias))
    end
    check("add_bias", [x, bias], check_add_bias)

    ma = Var.leaf(random_tensor(3, 5, 5))
    mb = Var.leaf(random_tensor(5, 4, 6))
    closure check_matmul()
      Autograd.sum(Autograd.matmul(ma, mb))
    end
    check("matmul", [ma, mb], check_matmul)

    sx = Var.leaf(random_tensor(3, 4, 7))
    closure check_scale()
      Autograd.sum(Autograd.scale(sx, 2.5))
    end
    check("scale", [sx], check_scale)

    tx = Var.leaf(random_tensor(3, 4, 8))
    closure check_transpose()
      Autograd.sum(Autograd.transpose(tx))
    end
    check("transpose", [tx], check_transpose)

    cx = Var.leaf(random_tensor(3, 6, 9))
    closure check_columns()
      Autograd.sum(Autograd.columns(cx, 2, 3))
    end
    check("columns", [cx], check_columns)

    ca = Var.leaf(random_tensor(3, 2, 10))
    cb = Var.leaf(random_tensor(3, 3, 11))
    closure check_concat()
      Autograd.sum(Autograd.concat_columns([ca, cb]))
    end
    check("concat_columns", [ca, cb], check_concat)

    lx = Var.leaf(random_tensor(4, 5, 12))
    lgamma = Var.leaf(random_tensor(1, 5, 13))
    lbeta = Var.leaf(random_tensor(1, 5, 14))
    closure check_layernorm()
      Autograd.sum(Autograd.layernorm(lx, lgamma, lbeta, 0.00001))
    end
    check("layernorm", [lx, lgamma, lbeta], check_layernorm)

    smx = Var.leaf(random_tensor(3, 5, 15))
    closure check_softmax()
      Autograd.sum(Autograd.softmax(smx))
    end
    check("softmax", [smx], check_softmax)

    gx = Var.leaf(random_tensor(3, 4, 16))
    closure check_gelu()
      Autograd.sum(Autograd.gelu(gx))
    end
    check("gelu", [gx], check_gelu)

    tt = Var.leaf(random_tensor(10, 4, 17))
    pt = Var.leaf(random_tensor(6, 4, 18))
    token_ids = [2, 5, 1]
    closure check_embedding()
      Autograd.sum(Autograd.embedding(tt, pt, token_ids))
    end
    check("embedding", [tt, pt], check_embedding)
  end
end

GradCheckRunner.run()
