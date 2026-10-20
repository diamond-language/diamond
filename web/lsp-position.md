# Columns: bytes in the compiler, UTF-16 units in the editor

*A first chapter, chosen because it is small (about seventy lines of C), complete, and has one idea worth a whole chapter.
It tangles to `lsp/position.h`.*

## The problem

Suppose a Diamond source line is

```
name = "café" + oops
```

and the compiler reports an error at `oops`. In Ruby you would ask the string for its index and be done:
`line.index("oops")` is `16`, counted in *characters*. Diamond's lexer does not count characters. It counts
**bytes**, because the C code walks a `char *` and `é` takes two bytes in UTF-8. So the lexer says `oops` starts at
byte `17`.

The editor speaks the Language Server Protocol, and the protocol counts neither. A position's `character` is measured
in **UTF-16 code units**, because JSON-RPC was designed alongside JavaScript, whose strings are UTF-16. For `café`
the answer would be `16` again, since every character in it fits in one UTF-16 unit. Put a character outside the Basic
Multilingual Plane on the line (an emoji, say) and it takes **two** units but **four** bytes, and now all three
numbers differ.

| text before `oops` | characters (Ruby's `index`) | bytes (the lexer) | UTF-16 units (the editor) |
|---|---|---|---|
| `name = "cafe" + ` | 16 | 16 | 16 |
| `name = "café" + ` | 16 | 17 | 16 |
| `name = "😀" + ` | 13 | 16 | 14 |

For ASCII all three agree, which is why the bug stayed hidden: hover and go-to-definition worked until someone put an
accented letter in a string. After that, every position on the rest of the line was wrong.

So we need two conversions, one in each direction, and they live in a header so that every part of the language server
can use them.

```c <<file: lsp/position.h>>=
#pragma once

#include <stddef.h>

/* Column conversion between the compiler (bytes) and the language server (UTF-16 units); see web/lsp-position.md. */

<<find a line>>

<<count UTF-16 units in a byte prefix>>

<<find the bytes covered by UTF-16 units>>

<<the two conversions the server calls>>
```

## C for a Ruby programmer: what you need to read this file

* `const char *text` and `size_t length` are how C passes a string: a pointer to the first byte and a separate length.
  There is no `String` object that knows its own size, so every function that reads text is told where it ends. A
  `size_t` is an unsigned integer big enough to index memory. Think of it as "a count or an index that cannot be
  negative".
* `static inline` on a function in a header means "this function belongs to whoever includes the header, and the
  compiler may paste its body in place of the call". It is how you get a small helper without a `.c` file to link.
* `nullptr` is C23's name for "no pointer". It is Ruby's `nil`, except that dereferencing it crashes instead of
  raising `NoMethodError`.
* A pointer parameter such as `size_t *line_length` is how a C function returns a **second** value. Ruby would return
  `[start, length]`; C writes the length into the caller's variable through the pointer.
* `unsigned char` is a byte read as a number from 0 to 255. Plain `char` may be signed, which makes the comparisons
  below wrong for bytes above 127, so we convert before comparing.

## Finding the line

Positions arrive as a 1-based line and a column on that line, so the first job is to find where line *n* begins. The
Ruby version is `text.lines[n - 1]`, which builds an array of every line just to read one. C walks the bytes once:

```c <<find a line>>=
/* Start of 1-based `line` in `text`, and its length without the newline. nullptr if the line does not exist. */
static inline const char *lsp_line_start(const char *text,size_t length,size_t line,size_t *line_length) {
    if(text==nullptr||line==0)return nullptr;
    size_t offset=0,current=1;
    <<skip forward to the start of the wanted line>>
    <<measure the wanted line>>
    return text+offset;
}
```

The function returns a pointer *into* the original text, not a copy; `text + offset` is Ruby's `text.byteslice(offset..)`
without allocating anything. The price is that the caller must not use it after the text is freed.

To skip forward, count newlines. Reaching the end of the text before finding enough of them means the line does not
exist:

```c <<skip forward to the start of the wanted line>>=
while(current<line) {
    while(offset<length&&text[offset]!='\n')offset++;
    if(offset>=length)return nullptr;
    offset++;
    current++;
}
```

Then find where this line ends, and hand its length back through the pointer parameter described above:

```c <<measure the wanted line>>=
size_t end=offset;
while(end<length&&text[end]!='\n')end++;
*line_length=end-offset;
```

## From bytes to UTF-16 units

Going from the compiler's number to the editor's is a count. Walk the first `byte_count` bytes of the line and add up
what each one contributes. UTF-8 makes this easy, because the first byte of every character announces what it is:

| first byte (binary) | meaning | UTF-16 units |
|---|---|---|
| `0xxxxxxx` | ASCII, one byte | 1 |
| `10xxxxxx` | **continuation** byte inside a multi-byte character | 0 (already counted) |
| `110xxxxx`, `1110xxxx` | first byte of a 2- or 3-byte character | 1 |
| `11110xxx` | first byte of a 4-byte character | 2 (a *surrogate pair*) |

The test `(byte & 0xC0) == 0x80` masks the top two bits and checks for the continuation pattern `10`, and `byte >= 0xF0`
recognises the four-byte lead. A column past the end of the line (the compiler pointing just after the last character)
is passed through unchanged, one unit per byte beyond the line, which is what an editor expects of a cursor at the end.

```c <<count UTF-16 units in a byte prefix>>=
/* UTF-16 code units in the first `byte_count` bytes of a UTF-8 line (stops at the end of the line). A
 * continuation byte adds nothing, a four-byte sequence is a surrogate pair, anything else is one unit. */
static inline size_t lsp_utf16_from_bytes(const char *line,size_t line_length,size_t byte_count) {
    size_t units=0;
    const size_t limit=byte_count<line_length?byte_count:line_length;
    for(size_t index=0;index<limit;index++) {
        const unsigned char byte=(unsigned char)line[index];
        if((byte&0xC0)==0x80)continue;
        units+=(byte>=0xF0)?2:1;
    }
    return units+(byte_count>line_length?byte_count-line_length:0);
}
```

## From UTF-16 units back to bytes

The other direction cannot just count bytes: it has to stop when it has covered the requested number of *units*, and
it must advance a whole character at a time, so it reads each lead byte to learn the character's length in bytes
(`sequence`) and its width in units (`width`).

```c <<find the bytes covered by UTF-16 units>>=
/* Bytes of a UTF-8 line covered by `utf16` code units. A position inside a surrogate pair rounds down to the
 * start of that character. Past the end of the line it is the line length. */
static inline size_t lsp_bytes_from_utf16(const char *line,size_t line_length,size_t utf16) {
    size_t units=0,index=0;
    while(index<line_length) {
        const unsigned char byte=(unsigned char)line[index];
        size_t sequence=1;
        size_t width=1;
        <<read the length of the character at this byte>>
        if(units+width>utf16)break;
        units+=width;
        index+=sequence;
    }
    return index<line_length?index:line_length;
}
```

The lead byte tells us everything, the same table as above read the other way:

```c <<read the length of the character at this byte>>=
if(byte>=0xF0){sequence=4;width=2;}
else if(byte>=0xE0)sequence=3;
else if(byte>=0xC0)sequence=2;
```

The `if(units+width>utf16)break;` line is the interesting one. A client may send a position that falls in the middle of
a surrogate pair (it counts units, and a pair is two). We refuse to split a character: the loop stops *before* it, so
the position rounds down to the start of that character rather than landing inside its bytes.

## The two functions the rest of the server calls

Everything above works on one line. The server holds a whole document and a (line, column) pair, so the two entry
points find the line and then convert. When the line cannot be found (for example the document is not loaded) they
return the number they were given, which is exactly what the server did before these functions existed, so the
failure mode is the old behaviour and not a crash.

```c <<the two conversions the server calls>>=
/* The LSP `character` for a 0-based byte column on 1-based `line` of `text`. */
static inline size_t lsp_character_for(const char *text,size_t length,size_t line,size_t byte_column) {
    size_t line_length=0;
    const char *start=lsp_line_start(text,length,line,&line_length);
    if(start==nullptr)return byte_column;
    return lsp_utf16_from_bytes(start,line_length,byte_column);
}

/* The 0-based byte column on 1-based `line` of `text` for an LSP `character`. */
static inline size_t lsp_byte_column_for(const char *text,size_t length,size_t line,size_t character) {
    size_t line_length=0;
    const char *start=lsp_line_start(text,length,line,&line_length);
    if(start==nullptr)return character;
    return lsp_bytes_from_utf16(start,line_length,character);
}
```

## Where this is tested

`tests/position_test.c` checks both directions on ASCII, accented letters, a character outside the Basic Multilingual
Plane, a position in the middle of a surrogate pair, a column past the end of the line, and a missing line.
`tests/lsp_test.sh` then drives the real server with non-ASCII documents.
