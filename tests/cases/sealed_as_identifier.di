# `sealed` is a keyword only right before `class`; elsewhere it's a name.
sealed class Shape
end
class Dot < Shape
end
def wrap(sealed: String) -> String = "[#{sealed}]"
sealed = wrap("box")
[sealed, Dot.new().is_a?(Shape)]
