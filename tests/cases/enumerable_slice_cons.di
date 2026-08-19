# each_slice: non-overlapping chunks, last one short if not an exact
# multiple. each_cons: overlapping sliding windows, always exactly
# `size` long.
"#{[1, 2, 3, 4, 5].each_slice(2)}, #{[1, 2, 3, 4, 5].each_cons(2)}"
