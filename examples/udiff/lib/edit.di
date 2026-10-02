# One line of an edit script. Edit is sealed, so every `case` over an Edit
# has to name Keep, Delete, and Insert (or have an `else`): adding a fourth
# kind of edit stops the diff, hunk, and patch code from compiling until
# each of them decides what to do with it.
sealed class Edit
  attr_reader text: String
  def initialize(text: String)
    @text = text
  end
end

# The line is unchanged: in both files.
class Keep < Edit
end

# The line is only in the OLD file.
class Delete < Edit
end

# The line is only in the NEW file.
class Insert < Edit
end

# The diff could not be computed (the files are too different to handle).
class DiffError < StandardError
end

# The unified-diff prefix for an edit: a space, "-", or "+".
def edit_prefix(edit: Edit) -> String
  case edit
  when Keep then " "
  when Delete then "-"
  when Insert then "+"
  end
end

# The same script read backwards: what was deleted is inserted, and the
# other way around. Applying a reversed patch undoes it.
def reverse_edit(edit: Edit) -> Edit
  case edit
  when Keep then edit
  when Delete then Insert.new(edit.text())
  when Insert then Delete.new(edit.text())
  end
end
