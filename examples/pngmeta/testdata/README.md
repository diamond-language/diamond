The PNGs here were generated with Python's zlib and struct (a few pixels
each): `photo.png` has text, time, and gamma chunks; `icon.png` has none;
`corrupt.png` is `photo.png` with one byte of a tEXt chunk changed after its
CRC was computed; `truncated.png` stops mid-chunk; `notpng.png` isn't a PNG.
