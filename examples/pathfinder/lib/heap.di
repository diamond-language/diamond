# A binary min-heap of (priority, cell) pairs. Ties break by insertion
# order, so equal-priority cells come out first-in first-out and a search
# behaves the same on every run.
struct Entry(priority: Int, order: Int, cell: Int)
end

# Stored in a flat Array as an implicit binary tree: the children of index i
# are 2i+1 and 2i+2, and its parent is (i-1)/2. The invariant is that no
# entry comes AFTER its parent, so the best entry is always at index 0.

class MinHeap
  def initialize()
    @items = []
    @pushed = 0
  end

  def empty?() = @items.empty?()

  def size() = @items.length()

  # Add at the end (the first free leaf), then float it up to its place.
  def push(priority: Int, cell: Int)
    @items.push(Entry.new(priority, @pushed, cell))
    @pushed += 1
    self.sift_up(@items.length() - 1)
  end

  # Removes and returns the entry with the lowest priority.
  def pop() -> Entry
    top = @items[0]

    # Take the last leaf out, and (unless it was the only entry) move it to
    # the root and sink it down. That keeps the tree complete.
    last = @items.pop()
    unless @items.empty?()
      @items[0] = last
      self.sift_down(0)
    end
    top
  end

  private

  # Heap order: lower priority first; equal priorities by insertion order.
  def before?(a: Entry, b: Entry) -> Bool
    a.priority() < b.priority() || (a.priority() == b.priority() && a.order() < b.order())
  end

  def swap(i: Int, j: Int)
    held = @items[i]
    @items[i] = @items[j]
    @items[j] = held
  end

  # Swap an entry with its parent while it belongs before it.
  def sift_up(start: Int)
    index = start

    while index > 0
      parent = (index - 1) / 2
      break unless self.before?(@items[index], @items[parent])
      self.swap(index, parent)
      index = parent
    end
  end

  # Swap an entry with its better child until neither child belongs before
  # it. `best` picks the winner among the entry itself and its two children.
  def sift_down(start: Int)
    index = start
    count = @items.length()

    loop do
      left = index * 2 + 1
      right = left + 1
      best = index
      best = left if left < count && self.before?(@items[left], @items[best])
      best = right if right < count && self.before?(@items[right], @items[best])
      break if best == index
      self.swap(index, best)
      index = best
    end
  end
end
