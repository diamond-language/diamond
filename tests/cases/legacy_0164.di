def run()
 def accepts(callback: Callable[[Int], String]) = callback(42)
 def stringify(value: Int) -> String = "#{value}"
 accepts(stringify)
end
run()
