def outer(value)
  def middle()
    def increment()
      value = value + 1
    end
    increment
  end
  middle
end

make_increment = outer(40)
increment = make_increment()
increment()
increment()
