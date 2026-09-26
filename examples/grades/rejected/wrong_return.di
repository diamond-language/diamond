# The interface promises label() -> String; returning the Int count is
# caught where the method is defined.
require "../lib/reports"
class CountReport
  def label() -> String = 42
end
