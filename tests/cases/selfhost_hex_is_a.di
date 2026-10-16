mask = 0xFF & 0b1010_1010
perm = 0o755
big = 0xCAFE_BABE
kinds = [5.is_a?(Int), "x".is_a?(String), 5.is_a?(String), nil.is_a?(Nil), [1].is_a?(Array)]
names = [5.class(), "s".class(), [].class(), nil.class()]
[mask, perm, big, kinds, names]
