# Result length matches the receiver's own length, not the argument's --
# the shorter array pads with Nil, matching Ruby's own zip.
"#{[1, 2, 3].zip([4, 5])}, #{[1, 2].zip([3, 4, 5])}"
