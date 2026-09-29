# A Huffman tree, built greedily: repeatedly take the two least-frequent
# nodes in the forest and merge them, until one tree remains. With at
# most 256 distinct byte values, a full sort every round (O(n^2 log n)
# overall) is simpler than a real priority queue and plenty fast here.

sealed class HuffNode
end

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

def huffman_build_tree(freqs: Hash) -> HuffNode
  forest = freqs.keys().map() do |byte| Leaf.new(byte, freqs[byte]) end
  while forest.length() > 1
    forest = forest.sort_by() do |node| node.freq() end
    merged = Branch.new(forest[0], forest[1], forest[0].freq() + forest[1].freq())
    forest = forest.slice(2, forest.length() - 2) + [merged]
  end
  forest[0]
end

# One code per byte value, as a String of '0'/'1' -- "0" itself for the
# single-symbol edge case (a one-node "tree", never actually a Branch).
def huffman_collect_codes(node: HuffNode, prefix: String, codes: Hash)
  case node
  when Leaf
    codes[node.byte()] = prefix.empty?() ? "0" : prefix
  when Branch
    huffman_collect_codes(node.left(), prefix + "0", codes)
    huffman_collect_codes(node.right(), prefix + "1", codes)
  end
end
