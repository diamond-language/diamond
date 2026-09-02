# Chops a whole tokenized corpus into fixed-length (input, target)
# training examples -- the model has no batching dimension (Tensor is
# 2D-only, see the README's own Scope section), so training is always
# one sequence at a time; this is what "one sequence" means when the
# actual source is a long corpus rather than a hand-written toy list.
class Dataset
  # token_ids: the whole tokenized corpus, as one flat Array of Int.
  # seq_len: window length -- target[i] is always input[i]'s next
  # token, so each window actually consumes seq_len+1 consecutive
  # corpus tokens. stride: how far to advance between windows --
  # `stride == seq_len` (the default any caller should pass unless it
  # specifically wants overlap) means non-overlapping windows, using
  # every token in the corpus exactly once per epoch; a smaller stride
  # reuses more of the corpus per epoch (more training examples from
  # the same text) at the cost of adjacent windows sharing most of
  # their content. Returns an Array of [input_ids, target_ids] pairs.
  def self.windows(token_ids, seq_len, stride)
    examples = []
    start = 0
    while start + seq_len < token_ids.length()
      input_ids = []
      target_ids = []
      i = 0
      while i < seq_len
        input_ids.push(token_ids[start + i])
        target_ids.push(token_ids[start + i + 1])
        i += 1
      end
      examples.push([input_ids, target_ids])
      start += stride
    end
    examples
  end
end
