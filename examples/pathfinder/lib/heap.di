# A binary min-heap of (priority, cell) pairs. Ties break by insertion
# order, so equal-priority cells come out first-in first-out and a search
# behaves the same on every run.
struct Entry(priority: Int, order: Int, cell: Int)
end

class MinHeap
  def initialize()
    @items = []
    @pushed = 0
  end

  def empty?() = @items.empty?()

  def size() = @items.length()

  def push(priority: Int, cell: Int)
    @items.push(Entry.new(priority, @pushed, cell))
    @pushed += 1
    self.sift_up(@items.length() - 1)
  end

  # Removes and returns the entry with the lowest priority.
  def pop() -> Entry
    top = @items[0]
    last = @items.pop()
    unless @items.empty?()
      @items[0] = last
      self.sift_down(0)
    end
    top
  end

  private

  def before?(a: Entry, b: Entry) -> Bool
    a.priority() < b.priority() || (a.priority() == b.priority() && a.order() < b.order())
  end

  def swap(i: Int, j: Int)
    held = @items[i]
    @items[i] = @items[j]
    @items[j] = held
  end

  def sift_up(start: Int)
    index = start
    while index > 0
      parent = (index - 1) / 2
      break unless self.before?(@items[index], @items[parent])
      self.swap(index, parent)
      index = parent
    end
  end

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
