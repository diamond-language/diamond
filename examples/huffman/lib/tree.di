# A Huffman tree, built greedily: repeatedly take the two least-frequent
# nodes in the forest and merge them, until one tree remains. With at
# most 256 distinct byte values, a full sort every round (O(n^2 log n)
# overall) is simpler than a real priority queue and plenty fast here.

sealed class HuffNode
end

# A node is either a Leaf (one byte value and how often it occurs) or a
# Branch joining two subtrees. `freq` on a Branch is the sum of its children.
class Leaf < HuffNode
  def initialize(byte: Int, freq: Int)
    @byte = byte
    @freq = freq
  end
  def byte() = @byte
  def freq() = @freq
end

class Branch < HuffNode
  def initialize(left: HuffNode, right: HuffNode, freq: Int)
    @left = left
    @right = right
    @freq = freq
  end
  def left() = @left
  def right() = @right
  def freq() = @freq
end

# Starts with one Leaf per byte value ("the forest"), then merges the two
# rarest into a Branch until a single tree is left. Rare bytes end up deep
# in the tree (long codes), common ones near the root (short codes), which
# is what makes the output smaller.
def huffman_build_tree(freqs: Hash) -> HuffNode
  forest = freqs.keys().map() do |byte| Leaf.new(byte, freqs[byte]) end

  while forest.length() > 1
    # Sort so the two lowest frequencies come first, merge them, and put the
    # merged Branch back in place of the two.
    forest = forest.sort_by() do |node| node.freq() end
    merged = Branch.new(forest[0], forest[1], forest[0].freq() + forest[1].freq())
    forest = forest.slice(2, forest.length() - 2) + [merged]
  end
  forest[0]
end

# One code per byte value, as a String of '0'/'1' -- "0" itself for the
# single-symbol edge case (a one-node "tree", never actually a Branch).
# Walks the tree, building each leaf's code from the path to it: "0" for each
# left turn, "1" for each right. `codes` is filled in (it is both input and
# output).
def huffman_collect_codes(node: HuffNode, prefix: String, codes: Hash)
  case node
  when Leaf
    codes[node.byte()] = prefix.empty?() ? "0" : prefix
  when Branch
    huffman_collect_codes(node.left(), prefix + "0", codes)
    huffman_collect_codes(node.right(), prefix + "1", codes)
  end
end
