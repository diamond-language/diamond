#include "json.h"
#include "rpc.h"
#include "message_length.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_number(const char *source, bool valid, double expected) {
    const char *error=nullptr;
    JsonValue *value=json_parse(source,strlen(source),&error);
    if(valid ? value==nullptr||value->kind!=JSON_NUMBER||value->as.number!=expected :
        value!=nullptr||error==nullptr) {
        fprintf(stderr,"json_test: unexpected number result for %s\n",source);
        failures++;
    }
    json_free(value);
}

static void expect_frame(const char *headers, bool valid) {
    FILE *stream=tmpfile();
    if(stream==nullptr) { failures++; return; }
    fputs(headers,stream);
    fputs("\r\n{}",stream);
    rewind(stream);
    const char *error=nullptr;
    JsonValue *value=rpc_read_message(stream,&error);
    if(valid ? value==nullptr||value->kind!=JSON_OBJECT : value!=nullptr||error==nullptr) {
        fprintf(stderr,"json_test: unexpected framing result for %s\n",headers);
        failures++;
    }
    json_free(value);
    fclose(stream);
}

static void expect_string(const char *label, const char *source, const char *expected) {
    const char *error = nullptr;
    JsonValue *value = json_parse(source, strlen(source), &error);
    const char *chars = nullptr;
    size_t length = 0;
    if (value == nullptr || !json_as_string(value, &chars, &length) ||
        length != strlen(expected) || memcmp(chars, expected, length) != 0) {
        fprintf(stderr, "json_test: %s: got %zu bytes, expected %zu\n", label,
                chars == nullptr ? (size_t)0 : length, strlen(expected));
        failures++;
    }
    json_free(value);
}

int main(void) {
    const char *invalid[]={"01","-01","1.","1e","1e+","1e-","1e999","-1e999"};
    for(size_t i=0;i<sizeof invalid/sizeof *invalid;i++)
        expect_number(invalid[i],false,0);
    expect_number("0",true,0);
    expect_number("-0",true,0);
    expect_number("-1.25e+2",true,-125);
    expect_number("1E-2",true,0.01);
    expect_frame("Content-Length: 2\r\n",true);
    expect_frame("Content-Length:\t2 \t\r\n",true);
    expect_frame("Content-Length: 2garbage\r\n",false);
    expect_frame("Content-Length: +2\r\n",false);
    expect_frame("Content-Length: -2\r\n",false);
    expect_frame("Content-Length: \r\n",false);
    expect_frame("Content-Length: 999999999999999999999999\r\n",false);
    expect_frame("Content-Length: 67108865\r\n",false);
    expect_frame("Content-Length: 2\r\nContent-Length: 2\r\n",false);
    expect_frame("Content-Type: application/json\r\n",false);
    size_t length=0;
    if(!diamond_parse_message_length("67108864",&length)||
        length!=DIAMOND_MAX_MESSAGE_LENGTH) failures++;
    /* Raw UTF-8 bytes in a JSON string are copied through, not re-encoded. */
    expect_string("raw 2-byte", "\"caf\xC3\xA9\"", "caf\xC3\xA9");
    expect_string("raw 3-byte", "\"\xE2\x82\xAC\"", "\xE2\x82\xAC");
    expect_string("raw 4-byte", "\"\xF0\x9F\x92\x8E\"", "\xF0\x9F\x92\x8E");
    /* Escapes still produce UTF-8. */
    expect_string("escaped", "\"caf\\u00e9\"", "caf\xC3\xA9");
    expect_string("surrogate pair", "\"\\ud83d\\udc8e\"", "\xF0\x9F\x92\x8E");
    expect_string("ascii", "\"plain\\n\"", "plain\n");
    if (failures == 0) puts("json_test: ok");
    return failures == 0 ? 0 : 1;
}
