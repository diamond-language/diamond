class Loss
  # logits: Var (seq_len x vocab_size). targets: Array of Int, length
  # seq_len -- targets[i] is the correct next-token id for position i.
  # Standard decoder-only-LM training signal: position i's logits
  # predict token i+1, so targets is normally token_ids shifted by one
  # (train.di builds that shift; this class only computes the loss for
  # whatever (logits, targets) it's given).
  #
  # Fused softmax+cross-entropy rather than composing Autograd.softmax
  # with a separate log/negative-log-likelihood op, for two reasons:
  # numerical stability (never computes log() of a near-zero
  # probability directly this way) and because the combined gradient
  # collapses to the well-known probs-minus-one-hot form, far simpler
  # than differentiating through softmax and log as two separate ops.
  def self.softmax_cross_entropy(logits, targets)
    seq_len = logits.tensor().rows()
    vocab_size = logits.tensor().cols()
    probs = tensor_clone(logits.tensor())
    tensor_row_softmax!(probs)

    total_loss = 0.0
    i = 0
    while i < seq_len
      target = targets[i]
      p = probs.get(i, target)
      # Clamp away from exactly 0 -- only reachable if a probability
      # underflows, which would otherwise make log() a domain error
      # rather than just an unhelpful training signal.
      if p < 0.0000000001
        p = 0.0000000001
      end
      total_loss += 0.0 - log(p)
      i += 1
    end
    mean_loss = total_loss / seq_len

    output = Tensor.zeros(1, 1)
    output.set(0, 0, mean_loss)
    result = Var.new(output, [logits], true)
    closure backward()
      if logits.requires_grad()
        i = 0
        while i < seq_len
          target = targets[i]
          j = 0
          while j < vocab_size
            indicator = if j == target then 1.0 else 0.0 end
            # d(mean cross-entropy)/d(logit_ij) = (probs_ij - indicator) / seq_len
            dx = (probs.get(i, j) - indicator) / seq_len
            logits.grad().set(i, j, logits.grad().get(i, j) + dx * result.grad().get(0, 0))
            j += 1
          end
          i += 1
        end
      end
    end
    result.backward_fn = backward
    result
  end
end
