class Widget
  def initialize(tag)
    @tag = tag
  end
  def tag() = @tag
  def render(items)
    # `v = self` used to be able to alias v's register directly to
    # self's own register 0 (register_is_named alone missed this, since
    # an ordinary method's `self` has no compiler->locals[] entry at
    # all). A block capturing v then boxed register 0 in place,
    # corrupting self for every later use in this same method.
    v = self
    labels = items.map() do |item|
      "#{v.tag()}:#{item}"
    end
    "#{self.tag()} -> #{labels.join(", ")}"
  end
end
puts(Widget.new("box").render([1, 2, 3]))
