#include "definition.h"
#include "diagnostics.h"
#include "document.h"
#include "document_symbol.h"
#include "hover.h"
#include "json.h"
#include "rpc.h"

#include <stdio.h>
#include <string.h>

/* A request id is always a JSON number or string per the LSP spec (never
 * an object/array/bool, and null only for a notification that never
 * reaches this function). Cloning it into a fresh, independently owned
 * value lets a response object own its own id without needing a general
 * json_clone -- the request's own id remains borrowed from `message` and
 * stays valid only until this loop iteration's json_free(message). */
static JsonValue *clone_id(const JsonValue *id) {
    if(id==nullptr)return json_null();
    if(id->kind==JSON_NUMBER)return json_number(id->as.number);
    if(id->kind==JSON_STRING)return json_string(id->as.string.chars,id->as.string.length);
    return json_null();
}

static void send_response(const JsonValue *request_id,JsonValue *result) {
    JsonValue *message=json_object();
    if(message==nullptr) {
        json_free(result);
        return;
    }
    json_object_set(message,"jsonrpc",json_string_z("2.0"));
    json_object_set(message,"id",clone_id(request_id));
    json_object_set(message,"result",result);
    rpc_write_message(stdout,message);
    json_free(message);
}

static void send_error(const JsonValue *request_id,int code,const char *error_message) {
    JsonValue *error=json_object();
    if(error==nullptr)return;
    json_object_set(error,"code",json_number(code));
    json_object_set(error,"message",json_string_z(error_message));
    JsonValue *message=json_object();
    if(message==nullptr) {
        json_free(error);
        return;
    }
    json_object_set(message,"jsonrpc",json_string_z("2.0"));
    json_object_set(message,"id",clone_id(request_id));
    json_object_set(message,"error",error);
    rpc_write_message(stdout,message);
    json_free(message);
}

static void send_notification(const char *method,JsonValue *params) {
    JsonValue *message=json_object();
    if(message==nullptr) {
        json_free(params);
        return;
    }
    json_object_set(message,"jsonrpc",json_string_z("2.0"));
    json_object_set(message,"method",json_string_z(method));
    json_object_set(message,"params",params);
    rpc_write_message(stdout,message);
    json_free(message);
}

static void publish_diagnostics(const char *uri,const char *text,size_t length) {
    JsonValue *diagnostics=diagnostics_compute(uri,text,length);
    if(diagnostics==nullptr)return;
    JsonValue *params=json_object();
    if(params==nullptr) {
        json_free(diagnostics);
        return;
    }
    json_object_set(params,"uri",json_string_z(uri));
    json_object_set(params,"diagnostics",diagnostics);
    send_notification("textDocument/publishDiagnostics",params);
}

static void handle_initialize(const JsonValue *id) {
    JsonValue *capabilities=json_object();
    JsonValue *result=json_object();
    if(capabilities==nullptr||result==nullptr) {
        json_free(capabilities);
        json_free(result);
        return;
    }
    /* TextDocumentSyncKind.Full = 1 -- every didChange carries the whole
     * document, not an incremental range edit. Simpler and sufficient
     * for a diagnostics-only server; see docs/lsp.md. */
    json_object_set(capabilities,"textDocumentSync",json_number(1));
    json_object_set(capabilities,"hoverProvider",json_bool(true));
    json_object_set(capabilities,"definitionProvider",json_bool(true));
    json_object_set(capabilities,"documentSymbolProvider",json_bool(true));
    json_object_set(result,"capabilities",capabilities);
    send_response(id,result);
}

/* Shared by handle_hover/handle_definition: extracts and validates
 * `textDocument.uri`/`position.line`/`position.character` from a
 * request's params, null-terminates the uri (json_as_string only ever
 * hands back a borrowed, non-null-terminated view into the parsed
 * message, same as `document_get_text` does for its own storage), and
 * looks the document up. Returns false (having already sent a `null`
 * response for a request id, if one was given) on any failure --
 * malformed params, uri too long, or the document isn't open -- so
 * callers can just `if(!extract...)return;`. */
static bool extract_document_position(DocumentTable *documents,const JsonValue *id,
        const JsonValue *params,char *uri_copy,size_t uri_copy_capacity,
        const char **text,size_t *text_length,size_t *line,size_t *character) {
    const JsonValue *text_document=json_object_get(params,"textDocument");
    const JsonValue *position=json_object_get(params,"position");
    const char *uri=nullptr;
    size_t uri_length=0;
    double line_value=0,character_value=0;
    if(!json_as_string(json_object_get(text_document,"uri"),&uri,&uri_length)||
       !json_as_number(json_object_get(position,"line"),&line_value)||
       !json_as_number(json_object_get(position,"character"),&character_value)||
       uri_length>=uri_copy_capacity) {
        send_response(id,json_null());
        return false;
    }
    memcpy(uri_copy,uri,uri_length);
    uri_copy[uri_length]='\0';
    *text=document_get_text(documents,uri_copy,text_length);
    if(*text==nullptr) {
        send_response(id,json_null());
        return false;
    }
    *line=(size_t)line_value;
    *character=(size_t)character_value;
    return true;
}

static void handle_hover(DocumentTable *documents,const JsonValue *id,
        const JsonValue *params) {
    char uri_copy[1024];
    const char *text=nullptr;
    size_t text_length=0,line=0,character=0;
    if(!extract_document_position(documents,id,params,uri_copy,sizeof uri_copy,
            &text,&text_length,&line,&character))
        return;
    JsonValue *result=hover_compute(uri_copy,text,text_length,line,character);
    if(result==nullptr) {
        send_response(id,json_null());
        return;
    }
    send_response(id,result);
}

static void handle_definition(DocumentTable *documents,const JsonValue *id,
        const JsonValue *params) {
    char uri_copy[1024];
    const char *text=nullptr;
    size_t text_length=0,line=0,character=0;
    if(!extract_document_position(documents,id,params,uri_copy,sizeof uri_copy,
            &text,&text_length,&line,&character))
        return;
    JsonValue *result=definition_compute(uri_copy,text,text_length,line,character);
    if(result==nullptr) {
        send_response(id,json_null());
        return;
    }
    send_response(id,result);
}

static void handle_document_symbol(DocumentTable *documents,const JsonValue *id,
        const JsonValue *params) {
    const JsonValue *text_document=json_object_get(params,"textDocument");
    const char *uri=nullptr;
    size_t uri_length=0;
    char uri_copy[1024];
    if(!json_as_string(json_object_get(text_document,"uri"),&uri,&uri_length)||
       uri_length>=sizeof uri_copy) {
        send_response(id,json_null());
        return;
    }
    memcpy(uri_copy,uri,uri_length);
    uri_copy[uri_length]='\0';
    size_t text_length=0;
    const char *text=document_get_text(documents,uri_copy,&text_length);
    if(text==nullptr) {
        send_response(id,json_null());
        return;
    }
    JsonValue *result=document_symbol_compute(uri_copy,text,text_length);
    if(result==nullptr) {
        send_response(id,json_null());
        return;
    }
    send_response(id,result);
}

static void handle_did_open(DocumentTable *documents,const JsonValue *params) {
    const JsonValue *text_document=json_object_get(params,"textDocument");
    const char *uri=nullptr,*text=nullptr;
    size_t uri_length=0,text_length=0;
    if(!json_as_string(json_object_get(text_document,"uri"),&uri,&uri_length))return;
    if(!json_as_string(json_object_get(text_document,"text"),&text,&text_length))return;
    if(!document_open(documents,uri,text,text_length))return;
    publish_diagnostics(uri,text,text_length);
}

static void handle_did_change(DocumentTable *documents,const JsonValue *params) {
    const JsonValue *text_document=json_object_get(params,"textDocument");
    const char *uri=nullptr;
    size_t uri_length=0;
    if(!json_as_string(json_object_get(text_document,"uri"),&uri,&uri_length))return;
    const JsonValue *changes=json_object_get(params,"contentChanges");
    if(changes==nullptr||changes->kind!=JSON_ARRAY||changes->as.array.count==0)return;
    /* Full sync means each entry is the complete new document text with
     * no range; a compliant client sends exactly one entry for Full
     * sync, but taking the last one if there's ever more than one costs
     * nothing and stays correct either way. */
    const JsonValue *last_change=changes->as.array.items[changes->as.array.count-1];
    const char *text=nullptr;
    size_t text_length=0;
    if(!json_as_string(json_object_get(last_change,"text"),&text,&text_length))return;
    if(!document_update(documents,uri,text,text_length))return;
    publish_diagnostics(uri,text,text_length);
}

static void handle_did_close(DocumentTable *documents,const JsonValue *params) {
    const JsonValue *text_document=json_object_get(params,"textDocument");
    const char *uri=nullptr;
    size_t uri_length=0;
    if(!json_as_string(json_object_get(text_document,"uri"),&uri,&uri_length))return;
    document_close(documents,uri);
    /* Publishing an empty diagnostics array is the spec's documented way
     * to clear whatever an editor was still showing for a file that's no
     * longer open. */
    JsonValue *diagnostics=json_array();
    if(diagnostics==nullptr)return;
    JsonValue *params_out=json_object();
    if(params_out==nullptr) {
        json_free(diagnostics);
        return;
    }
    json_object_set(params_out,"uri",json_string_z(uri));
    json_object_set(params_out,"diagnostics",diagnostics);
    send_notification("textDocument/publishDiagnostics",params_out);
}

int main(void) {
    DocumentTable *documents=document_table_create();
    if(documents==nullptr)return 1;
    bool shutdown_requested=false;
    while(true) {
        const char *read_error=nullptr;
        JsonValue *message=rpc_read_message(stdin,&read_error);
        if(message==nullptr) {
            if(read_error!=nullptr)fprintf(stderr,"diamond-lsp: %s\n",read_error);
            break;
        }
        const JsonValue *id=json_object_get(message,"id");
        const JsonValue *params=json_object_get(message,"params");
        const char *method_chars=nullptr;
        size_t method_length=0;
        json_as_string(json_object_get(message,"method"),&method_chars,&method_length);
        char method[128]={0};
        if(method_length>0&&method_length<sizeof method)
            memcpy(method,method_chars,method_length);

        if(strcmp(method,"initialize")==0) {
            handle_initialize(id);
        } else if(strcmp(method,"initialized")==0) {
            /* no-op notification */
        } else if(strcmp(method,"shutdown")==0) {
            shutdown_requested=true;
            send_response(id,json_null());
        } else if(strcmp(method,"exit")==0) {
            json_free(message);
            document_table_free(documents);
            return shutdown_requested?0:1;
        } else if(strcmp(method,"textDocument/didOpen")==0) {
            handle_did_open(documents,params);
        } else if(strcmp(method,"textDocument/didChange")==0) {
            handle_did_change(documents,params);
        } else if(strcmp(method,"textDocument/didClose")==0) {
            handle_did_close(documents,params);
        } else if(strcmp(method,"textDocument/hover")==0) {
            handle_hover(documents,id,params);
        } else if(strcmp(method,"textDocument/definition")==0) {
            handle_definition(documents,id,params);
        } else if(strcmp(method,"textDocument/documentSymbol")==0) {
            handle_document_symbol(documents,id,params);
        } else if(id!=nullptr) {
            send_error(id,-32601,"method not found");
        }
        json_free(message);
    }
    document_table_free(documents);
    return 0;
}
