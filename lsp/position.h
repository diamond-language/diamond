#pragma once

#include <stddef.h>

/* Positions. The lexer and compiler count a column in bytes within the line (1-based); the language server
 * protocol counts the `character` of a position in UTF-16 code units (0-based) unless a client negotiates
 * another encoding. They agree only on ASCII: after a multi-byte character on the line every column differs,
 * so a position taken from the editor selects the wrong token and a range sent back lands in the wrong place.
 * These convert in both directions. A text pointer of nullptr means the source is unavailable, and the byte
 * count is returned unchanged -- the behavior before these existed. */

/* Start of 1-based `line` in `text`, and its length without the newline. nullptr if the line does not exist. */
static inline const char *lsp_line_start(const char *text,size_t length,size_t line,size_t *line_length) {
    if(text==nullptr||line==0)return nullptr;
    size_t offset=0,current=1;
    while(current<line) {
        while(offset<length&&text[offset]!='\n')offset++;
        if(offset>=length)return nullptr;
        offset++;
        current++;
    }
    size_t end=offset;
    while(end<length&&text[end]!='\n')end++;
    *line_length=end-offset;
    return text+offset;
}

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

/* Bytes of a UTF-8 line covered by `utf16` code units. A position inside a surrogate pair rounds down to the
 * start of that character. Past the end of the line it is the line length. */
static inline size_t lsp_bytes_from_utf16(const char *line,size_t line_length,size_t utf16) {
    size_t units=0,index=0;
    while(index<line_length) {
        const unsigned char byte=(unsigned char)line[index];
        size_t sequence=1;
        size_t width=1;
        if(byte>=0xF0){sequence=4;width=2;}
        else if(byte>=0xE0)sequence=3;
        else if(byte>=0xC0)sequence=2;
        if(units+width>utf16)break;
        units+=width;
        index+=sequence;
    }
    return index<line_length?index:line_length;
}

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
