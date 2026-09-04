#include "completion.h"
#include "definition.h"
#include "dependencies.h"
#include "diagnostics.h"
#include "document.h"
#include "document_symbol.h"
#include "hover.h"
#include "json.h"
#include "references.h"
#include "rpc.h"
#include "workspace_symbol.h"

#include <stdio.h>
#include <stdlib.h>
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

/* Computes and publishes diagnostics for exactly `uri`, and refreshes
 * `dependencies`' own record of what `uri` currently requires. Doesn't
 * look at who *depends on* `uri` -- see publish_diagnostics below for
 * that; this is the non-cascading building block it (and the cascade's
 * own per-dependent republish, which must not itself cascade further --
 * see dependency_table_dependents' own comment on why one lookup
 * already covers arbitrarily deep chains) both call. */
static void publish_diagnostics_single(DependencyTable *dependencies,
        const DocumentTable *documents,const char *uri,const char *text,size_t length) {
    JsonValue *dependency_publish=nullptr;
    JsonValue *dependency_paths=nullptr;
    JsonValue *diagnostics=diagnostics_compute(documents,uri,text,length,
        &dependency_publish,&dependency_paths);
    if(diagnostics==nullptr) {
        json_free(dependency_publish);
        json_free(dependency_paths);
        return;
    }
    JsonValue *params=json_object();
    if(params==nullptr) {
        json_free(diagnostics);
        json_free(dependency_publish);
        json_free(dependency_paths);
        return;
    }
    json_object_set(params,"uri",json_string_z(uri));
    json_object_set(params,"diagnostics",diagnostics);
    send_notification("textDocument/publishDiagnostics",params);
    /* A didOpen/didChange for the *requesting* document can surface a
     * problem in a file it require's -- see diagnostics_compute's own
     * comment (lsp/diagnostics.h) for exactly when this fires. */
    if(dependency_publish!=nullptr)
        send_notification("textDocument/publishDiagnostics",dependency_publish);
    dependency_table_update(dependencies,uri,dependency_paths);
    json_free(dependency_paths);
}

/* Publishes `uri`'s own diagnostics, then -- since `uri` might itself
 * be a `require`d dependency of some *other* open document -- looks up
 * who currently depends on it (dependency_table_dependents) and
 * refreshes each of theirs too, now that document_resolve_source
 * (lsp/document.h) will see `uri`'s just-updated live buffer instead
 * of stale on-disk content the next time their own require resolves.
 * This is what makes editing an open dependency actually propagate --
 * see docs/lsp.md's former "editing the dependency directly... doesn't
 * re-trigger" gap. */
static void publish_diagnostics(DocumentTable *documents,DependencyTable *dependencies,
        const char *uri,const char *text,size_t length) {
    publish_diagnostics_single(dependencies,documents,uri,text,length);
    char *path=diagnostics_uri_to_path(uri);
    if(path==nullptr)return;
    size_t dependent_count=0;
    char **dependents=dependency_table_dependents(dependencies,path,&dependent_count);
    free(path);
    for(size_t index=0;index<dependent_count;index++) {
        size_t dependent_length=0;
        const char *dependent_text=document_get_text(documents,dependents[index],
            &dependent_length);
        if(dependent_text!=nullptr)
            publish_diagnostics_single(dependencies,documents,dependents[index],
                dependent_text,dependent_length);
        free(dependents[index]);
    }
    free(dependents);
}

/* Extracts the workspace root directory from `initialize`'s own params
 * -- `workspaceFolders[0].uri` (current spec) if present, else the
 * older `rootUri`, decoded via diagnostics_uri_to_path the same way
 * every other lsp/ feature turns a client-given uri into an on-disk
 * path. Leaves `out_root` untouched (caller should zero-init it
 * first) if neither is present/decodable -- workspace_symbol_compute
 * treats an empty root as "nothing to search" rather than failing. */
static void extract_workspace_root(const JsonValue *params,char *out_root,
        size_t out_root_capacity) {
    const JsonValue *folders=json_object_get(params,"workspaceFolders");
    const char *uri=nullptr;size_t uri_length=0;
    if(folders!=nullptr&&folders->kind==JSON_ARRAY&&folders->as.array.count>0) {
        const JsonValue *first=folders->as.array.items[0];
        json_as_string(json_object_get(first,"uri"),&uri,&uri_length);
    }
    if(uri==nullptr)
        json_as_string(json_object_get(params,"rootUri"),&uri,&uri_length);
    if(uri==nullptr||uri_length>=1024)return;
    char uri_copy[1024];
    memcpy(uri_copy,uri,uri_length);
    uri_copy[uri_length]='\0';
    char *path=diagnostics_uri_to_path(uri_copy);
    if(path==nullptr)return;
    (void)snprintf(out_root,out_root_capacity,"%s",path);
    free(path);
}

static void handle_initialize(const JsonValue *id,const JsonValue *params,
        char *workspace_root,size_t workspace_root_capacity) {
    extract_workspace_root(params,workspace_root,workspace_root_capacity);
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
    /* CompletionOptions -- an empty object (no triggerCharacters) is a
     * valid, complete value per the spec: this server doesn't need a
     * trigger character, since it returns the same full candidate
     * list regardless of what's already typed (see completion.h). */
    JsonValue *completion_options=json_object();
    if(completion_options!=nullptr)
        json_object_set(capabilities,"completionProvider",completion_options);
    json_object_set(capabilities,"workspaceSymbolProvider",json_bool(true));
    json_object_set(capabilities,"referencesProvider",json_bool(true));
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
    JsonValue *result=hover_compute(documents,uri_copy,text,text_length,line,character);
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
    JsonValue *result=definition_compute(documents,uri_copy,text,text_length,line,character);
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
    JsonValue *result=document_symbol_compute(documents,uri_copy,text,text_length);
    if(result==nullptr) {
        send_response(id,json_null());
        return;
    }
    send_response(id,result);
}

static void handle_completion(DocumentTable *documents,const JsonValue *id,
        const JsonValue *params) {
    char uri_copy[1024];
    const char *text=nullptr;
    size_t text_length=0,line=0,character=0;
    if(!extract_document_position(documents,id,params,uri_copy,sizeof uri_copy,
            &text,&text_length,&line,&character))
        return;
    JsonValue *result=completion_compute(documents,uri_copy,text,text_length,line,character);
    if(result==nullptr) {
        send_response(id,json_null());
        return;
    }
    send_response(id,result);
}

static void handle_workspace_symbol(DocumentTable *documents,const char *workspace_root,
        const JsonValue *id,const JsonValue *params) {
    const char *query_chars=nullptr;size_t query_length=0;
    json_as_string(json_object_get(params,"query"),&query_chars,&query_length);
    char query[256]={0};
    if(query_length>0&&query_length<sizeof query)
        memcpy(query,query_chars,query_length);
    JsonValue *result=workspace_symbol_compute(documents,workspace_root,query);
    if(result==nullptr) {
        send_response(id,json_null());
        return;
    }
    send_response(id,result);
}

static void handle_references(DocumentTable *documents,const char *workspace_root,
        const JsonValue *id,const JsonValue *params) {
    char uri_copy[1024];
    const char *text=nullptr;
    size_t text_length=0,line=0,character=0;
    if(!extract_document_position(documents,id,params,uri_copy,sizeof uri_copy,
            &text,&text_length,&line,&character))
        return;
    JsonValue *result=references_compute(documents,workspace_root,uri_copy,text,
        text_length,line,character);
    if(result==nullptr) {
        send_response(id,json_null());
        return;
    }
    send_response(id,result);
}

static void handle_did_open(DocumentTable *documents,DependencyTable *dependencies,
        const JsonValue *params) {
    const JsonValue *text_document=json_object_get(params,"textDocument");
    const char *uri=nullptr,*text=nullptr;
    size_t uri_length=0,text_length=0;
    if(!json_as_string(json_object_get(text_document,"uri"),&uri,&uri_length))return;
    if(!json_as_string(json_object_get(text_document,"text"),&text,&text_length))return;
    if(!document_open(documents,uri,text,text_length))return;
    publish_diagnostics(documents,dependencies,uri,text,text_length);
}

static void handle_did_change(DocumentTable *documents,DependencyTable *dependencies,
        const JsonValue *params) {
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
    publish_diagnostics(documents,dependencies,uri,text,text_length);
}

static void handle_did_close(DocumentTable *documents,DependencyTable *dependencies,
        const JsonValue *params) {
    const JsonValue *text_document=json_object_get(params,"textDocument");
    const char *uri=nullptr;
    size_t uri_length=0;
    if(!json_as_string(json_object_get(text_document,"uri"),&uri,&uri_length))return;
    document_close(documents,uri);
    dependency_table_remove_document(dependencies,uri);
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
    DependencyTable *dependencies=dependency_table_create();
    if(documents==nullptr||dependencies==nullptr) {
        document_table_free(documents);
        dependency_table_free(dependencies);
        return 1;
    }
    bool shutdown_requested=false;
    char workspace_root[1024]={0};
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
            handle_initialize(id,params,workspace_root,sizeof workspace_root);
        } else if(strcmp(method,"initialized")==0) {
            /* no-op notification */
        } else if(strcmp(method,"shutdown")==0) {
            shutdown_requested=true;
            send_response(id,json_null());
        } else if(strcmp(method,"exit")==0) {
            json_free(message);
            document_table_free(documents);
            dependency_table_free(dependencies);
            return shutdown_requested?0:1;
        } else if(strcmp(method,"textDocument/didOpen")==0) {
            handle_did_open(documents,dependencies,params);
        } else if(strcmp(method,"textDocument/didChange")==0) {
            handle_did_change(documents,dependencies,params);
        } else if(strcmp(method,"textDocument/didClose")==0) {
            handle_did_close(documents,dependencies,params);
        } else if(strcmp(method,"textDocument/hover")==0) {
            handle_hover(documents,id,params);
        } else if(strcmp(method,"textDocument/definition")==0) {
            handle_definition(documents,id,params);
        } else if(strcmp(method,"textDocument/documentSymbol")==0) {
            handle_document_symbol(documents,id,params);
        } else if(strcmp(method,"textDocument/completion")==0) {
            handle_completion(documents,id,params);
        } else if(strcmp(method,"workspace/symbol")==0) {
            handle_workspace_symbol(documents,workspace_root,id,params);
        } else if(strcmp(method,"textDocument/references")==0) {
            handle_references(documents,workspace_root,id,params);
        } else if(id!=nullptr) {
            send_error(id,-32601,"method not found");
        }
        json_free(message);
    }
    document_table_free(documents);
    dependency_table_free(dependencies);
    return 0;
}
