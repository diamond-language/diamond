# Text as lines, the way diff and patch see it: no line endings, no empty
# last line for a file that ends in a newline, and no lines at all for an
# empty file. (String#split keeps every empty piece, so the ends need
# trimming by hand.)
def split_lines(text: String) -> Array[String]
  return [] if text.empty?()
  lines = text.split("\n")
  lines.pop() if text.end_with?("\n")
  lines
end
