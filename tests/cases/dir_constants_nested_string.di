# __dir__ after a string holding a brace inside an interpolation is still substituted,
# and the literal text __dir__ after the interpolation is not.
table = {"}": "closing", "{": "opening"}
text = "#{table["}"]} and #{table["{"]} then __dir__ #{File.basename(__dir__).length() > 0}"
[text, File.exist?("#{table["}"].length() > 0 ? __dir__ : "/nonexistent"}/dir_constants_nested_string.di")]
