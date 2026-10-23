def outer(value)
  def middle()
    def inner()
      value = value + 1
    end
    inner
  end
  middle
end

make = outer(40)
increment = make()
increment()
increment()
