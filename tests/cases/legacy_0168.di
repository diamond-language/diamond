def run()
 def accepts(callback: Callable[[], String]) = callback()
 def greeting() -> String = "diamond"
 accepts(greeting)
end
run()
