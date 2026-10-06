#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
/* _GNU_SOURCE (a superset of _DEFAULT_SOURCE): only for pthread_getattr_np,
 * used to learn a thread's own native stack bounds for the ASan
 * fiber-switch annotations below. */
#define _GNU_SOURCE

#include "vm.h"
#include "jit.h"
#include "bignum.h"
#include "compiler.h"
#include "disassemble.h"
#include "loader.h"
#include "prelude.h"
#include "vm_internal.h"

/* <crypt.h> exists on glibc (libxcrypt) and musl (see BCrypt.hash's own
 * comment below for what musl's version lacks), declaring crypt_r/
 * struct crypt_data/CRYPT_GENSALT_* -- but not on FreeBSD, which declares
 * plain crypt()/crypt_r() directly in <unistd.h> (already included below)
 * instead, with no separate header at all. __has_include, not an
 * __APPLE__/__FreeBSD__-style OS check (see docs/portability.md's own
 * "What hasn't been found" on why this codebase avoids those): this is a
 * feature test, and the same reasoning applies wherever else a libc omits
 * this header. */
#if __has_include(<crypt.h>)
#include <crypt.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#include <math.h>
#include <stdckdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <spawn.h>
#include <sqlite3.h>
#include <libpq-fe.h>
#include <mysql.h>
#include <zlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vm_internal.h"

void mysql_library_init_once_fn(void) { mysql_library_init(0,nullptr,nullptr); }

/* Binds one Diamond value at a 1-indexed sqlite3 parameter position.
 * Factored out of the positional/named bind helpers below (identical
 * type-to-bind-call mapping either way, just a different way of
 * computing `position`). An unsupported Diamond value type is
 * DIAMOND_VM_TYPE_ERROR (a Diamond-level call-shape mistake, not
 * anything sqlite3 itself rejected); only a genuine sqlite3_bind_*
 * failure (rare -- effectively just OOM) is DIAMOND_VM_SQLITE3_ERROR. */
static DiamondVmStatus sqlite3_bind_one_value_helper(DiamondVm *vm,sqlite3_stmt *stmt,
        int position,DiamondValue value) {
    int rc=SQLITE_OK;
    if(value.kind==DIAMOND_VALUE_NIL) {
        rc=sqlite3_bind_null(stmt,position);
    } else if(value.kind==DIAMOND_VALUE_INT) {
        rc=sqlite3_bind_int64(stmt,position,value.as.integer);
    } else if(value.kind==DIAMOND_VALUE_FLOAT) {
        rc=sqlite3_bind_double(stmt,position,value.as.real);
    } else if(value.kind==DIAMOND_VALUE_BOOL) {
        rc=sqlite3_bind_int64(stmt,position,value.as.boolean?1:0);
    } else if(value.kind==DIAMOND_VALUE_OBJECT&&
              value.as.object->kind==DIAMOND_OBJECT_STRING) {
        const DiamondString *string=(const DiamondString *)value.as.object;
        rc=sqlite3_bind_text(stmt,position,string->chars,
            (int)string->length,SQLITE_TRANSIENT);
    } else {
        snprintf(vm->error,sizeof vm->error,
            "unsupported SQLite3 parameter type at position %d",position);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(rc!=SQLITE_OK) {
        snprintf(vm->error,sizeof vm->error,"%s",
            sqlite3_errmsg(sqlite3_db_handle(stmt)));
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    return DIAMOND_VM_OK;
}

/* Bind an Array of Diamond values positionally (1-indexed, sqlite3's own
 * convention for '?' placeholders) to `stmt`. A parameter-count mismatch
 * is DIAMOND_VM_ARITY_ERROR (surfaces as ArgumentError, the same class
 * an ordinary wrong-argument-count call already gets) rather than
 * SQLite3Error, since it's a Diamond-level call-shape mistake. No Diamond
 * allocation happens anywhere in here, so there's no GC-rooting concern
 * for `values`. */
static DiamondVmStatus sqlite3_bind_positional_params_helper(DiamondVm *vm,sqlite3_stmt *stmt,
        const DiamondValue *values,size_t count) {
    const int expected=sqlite3_bind_parameter_count(stmt);
    if(count!=(size_t)expected) {
        snprintf(vm->error,sizeof vm->error,
            "SQLite3 statement expects %d bound parameter(s), got %zu",expected,count);
        return DIAMOND_VM_ARITY_ERROR;
    }
    for(size_t index=0;index<count;index++) {
        const DiamondVmStatus bind_status=
            sqlite3_bind_one_value_helper(vm,stmt,(int)index+1,values[index]);
        if(bind_status!=DIAMOND_VM_OK)return bind_status;
    }
    return DIAMOND_VM_OK;
}

/* Bind a Hash of Diamond values by name (sqlite3's `:name`/`@name`/`$name`
 * placeholders -- only the `:name` spelling is supported at the Diamond
 * level, matching the sigil-free key convention most host-language sqlite3
 * drivers already use) to `stmt`. Same strictness as the positional path:
 * the Hash's size must exactly match the statement's own declared
 * parameter count, so a typo'd or missing key surfaces immediately as
 * ArgumentError rather than silently binding NULL for whatever sqlite3
 * itself leaves unbound. `hash->count`/`->entries[]` iterated the same
 * dense 0..count-1 way every other native Hash-consuming helper in this
 * file already does (see compile_method's own bound_values_hash walk). */
static DiamondVmStatus sqlite3_bind_named_params_helper(DiamondVm *vm,sqlite3_stmt *stmt,
        const DiamondHash *params) {
    const int expected=sqlite3_bind_parameter_count(stmt);
    if(params->count!=(size_t)expected) {
        snprintf(vm->error,sizeof vm->error,
            "SQLite3 statement expects %d bound parameter(s), got %zu",expected,params->count);
        return DIAMOND_VM_ARITY_ERROR;
    }
    for(size_t index=0;index<params->count;index++) {
        const DiamondValue key=params->entries[index].key;
        if(key.kind!=DIAMOND_VALUE_OBJECT||key.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "SQLite3 named params Hash keys must be Strings");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *key_string=(const DiamondString *)key.as.object;
        char *placeholder=malloc(key_string->length+2);
        if(placeholder==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        placeholder[0]=':';
        memcpy(placeholder+1,key_string->chars,key_string->length);
        placeholder[key_string->length+1]='\0';
        const int position=sqlite3_bind_parameter_index(stmt,placeholder);
        if(position==0) {
            snprintf(vm->error,sizeof vm->error,
                "SQLite3 statement has no parameter named '%s'",placeholder);
            free(placeholder);
            return DIAMOND_VM_ARITY_ERROR;
        }
        free(placeholder);
        const DiamondVmStatus bind_status=
            sqlite3_bind_one_value_helper(vm,stmt,position,params->entries[index].value);
        if(bind_status!=DIAMOND_VM_OK)return bind_status;
    }
    return DIAMOND_VM_OK;
}

/* `params` is Nil (no bind parameters), an Array (positional `?` binds),
 * or a Hash (named `:name` binds) -- anything else is a Diamond-level
 * call-shape mistake, TypeError. Shared by both the one-shot #execute/
 * #query prepare step and Statement#execute/#query's own reset-and-rebind
 * step below. */
static DiamondVmStatus sqlite3_bind_helper(DiamondVm *vm,sqlite3_stmt *stmt,
        DiamondValue params) {
    if(params.kind==DIAMOND_VALUE_NIL)
        return sqlite3_bind_positional_params_helper(vm,stmt,nullptr,0);
    if(params.kind==DIAMOND_VALUE_OBJECT&&params.as.object->kind==DIAMOND_OBJECT_ARRAY) {
        const DiamondArray *array=(const DiamondArray *)params.as.object;
        return sqlite3_bind_positional_params_helper(vm,stmt,array->values,array->count);
    }
    if(params.kind==DIAMOND_VALUE_OBJECT&&params.as.object->kind==DIAMOND_OBJECT_HASH)
        return sqlite3_bind_named_params_helper(vm,stmt,(const DiamondHash *)params.as.object);
    snprintf(vm->error,sizeof vm->error,"SQLite3 params argument must be an Array or a Hash");
    return DIAMOND_VM_TYPE_ERROR;
}

/* Prepare+single-statement-guard, with no bind step -- shared by the
 * one-shot path below (which binds immediately after) and SQLite3#prepare
 * (which returns the unbound statement itself; binding happens once per
 * later #execute/#query call, see sqlite3_statement_bind_and_reset_helper).
 * sqlite3_prepare_v2 only compiles the first statement up to a ';' and
 * leaves *pzTail pointing at whatever follows -- silently ignoring a
 * second statement chained after it would be a real correctness trap, so
 * anything left in the tail besides trailing whitespace is rejected
 * outright rather than dropped. On any failure path the statement is
 * finalized here (never left for the caller to clean up), so a caller
 * only ever needs to finalize the stmt it actually got back on
 * DIAMOND_VM_OK. */
static DiamondVmStatus sqlite3_prepare_only_helper(DiamondVm *vm,sqlite3 *db,
        const DiamondString *sql,sqlite3_stmt **out_stmt) {
    sqlite3_stmt *stmt=nullptr;
    const char *tail=nullptr;
    const int rc=sqlite3_prepare_v2(db,sql->chars,(int)sql->length,&stmt,&tail);
    if(rc!=SQLITE_OK) {
        snprintf(vm->error,sizeof vm->error,"%s",sqlite3_errmsg(db));
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    if(stmt==nullptr) {
        snprintf(vm->error,sizeof vm->error,"SQLite3: empty SQL statement");
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    const char *cursor=tail;
    while(*cursor==' '||*cursor=='\t'||*cursor=='\n'||*cursor=='\r')cursor++;
    if(*cursor!='\0') {
        sqlite3_finalize(stmt);
        snprintf(vm->error,sizeof vm->error,
            "SQLite3#execute/#query/#prepare only support one statement per call");
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    *out_stmt=stmt;
    return DIAMOND_VM_OK;
}

/* Prepare+bind in one step, behind the one-shot #execute/#query path. */
static DiamondVmStatus sqlite3_prepare_helper(DiamondVm *vm,sqlite3 *db,
        const DiamondString *sql,DiamondValue params,sqlite3_stmt **out_stmt) {
    sqlite3_stmt *stmt=nullptr;
    const DiamondVmStatus prepare_status=sqlite3_prepare_only_helper(vm,db,sql,&stmt);
    if(prepare_status!=DIAMOND_VM_OK)return prepare_status;
    const DiamondVmStatus bind_status=sqlite3_bind_helper(vm,stmt,params);
    if(bind_status!=DIAMOND_VM_OK) {
        sqlite3_finalize(stmt);
        return bind_status;
    }
    *out_stmt=stmt;
    return DIAMOND_VM_OK;
}

/* A PRAGMA statement is exempt from the query-shaped rejection below:
 * unlike a SELECT, `PRAGMA journal_mode = WAL`/`PRAGMA busy_timeout =
 * 5000`-style set-pragmas are legitimately run through #execute (they
 * change connection-level state, not read application data) even
 * though SQLite also happens to echo the resulting value back as a
 * one-row, one-column result set -- sqlite3_column_count can't tell
 * those apart from a real SELECT, so this checks the statement's own
 * SQL text instead. sqlite3_sql returns the exact text #prepare was
 * given, not attacker/input-controlled, so a simple prefix scan (skip
 * leading whitespace, case-insensitive "pragma") is safe and doesn't
 * need to handle SQL comments or other exotic prefixes no caller here
 * actually uses. */
static bool sqlite3_statement_is_pragma_helper(sqlite3_stmt *stmt) {
    const char *sql=sqlite3_sql(stmt);
    if(sql==nullptr)return false;
    while(*sql==' '||*sql=='\t'||*sql=='\n'||*sql=='\r')sql++;
    static const char keyword[]="pragma";
    for(size_t index=0;index<sizeof(keyword)-1;index++) {
        if(sql[index]=='\0')return false;
        if(tolower((unsigned char)sql[index])!=keyword[index])return false;
    }
    return true;
}

/* Steps an already-prepared, already-bound statement to completion,
 * discarding any rows -- the shared tail of #execute (one-shot, caller
 * finalizes after) and Statement#execute (reusable, caller never
 * finalizes). Returns the number of rows changed (sqlite3_changes),
 * the useful return value for INSERT/UPDATE/DELETE/DDL.
 *
 * A query-shaped statement (SELECT, or any other statement that
 * produces a result set -- sqlite3_column_count is nonzero once
 * prepared, regardless of whether it ever actually returns a row) is
 * rejected outright here rather than silently discarding its rows and
 * returning sqlite3_changes -- a leftover write-count from whatever
 * INSERT/UPDATE/DELETE this same connection last ran, completely
 * unrelated to the query just issued. Found the hard way: an #execute
 * call on a SELECT looks like it works (returns a plausible small
 * Int, no error) and returns a *coincidentally* plausible-looking but
 * wrong answer instead of the query's real result. #query (or
 * Statement#query) is the call that actually collects rows. PRAGMA is
 * exempted -- see sqlite3_statement_is_pragma_helper. */
static DiamondVmStatus sqlite3_run_to_completion_helper(DiamondVm *vm,sqlite3 *db,
        sqlite3_stmt *stmt,DiamondValue *result) {
    if(sqlite3_column_count(stmt)!=0&&!sqlite3_statement_is_pragma_helper(stmt)) {
        snprintf(vm->error,sizeof vm->error,
            "SQLite3#execute can't run a statement that returns rows "
            "(got %d column(s)) -- use #query instead",
            sqlite3_column_count(stmt));
        return DIAMOND_VM_TYPE_ERROR;
    }
    int rc=sqlite3_step(stmt);
    while(rc==SQLITE_ROW)rc=sqlite3_step(stmt); /* discard any rows */
    if(rc!=SQLITE_DONE) {
        snprintf(vm->error,sizeof vm->error,"%s",sqlite3_errmsg(db));
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    *result=DIAMOND_INT(sqlite3_changes(db));
    return DIAMOND_VM_OK;
}

static DiamondVmStatus sqlite3_execute_helper(DiamondVm *vm,DiamondSqlite3Handle *handle,
        const DiamondString *sql,DiamondValue params,DiamondValue *result) {
    sqlite3_stmt *stmt=nullptr;
    const DiamondVmStatus prepare_status=
        sqlite3_prepare_helper(vm,handle->db,sql,params,&stmt);
    if(prepare_status!=DIAMOND_VM_OK)return prepare_status;
    const DiamondVmStatus run_status=
        sqlite3_run_to_completion_helper(vm,handle->db,stmt,result);
    sqlite3_finalize(stmt);
    return run_status;
}

/* SQLITE_TEXT/SQLITE_BLOB both go through allocate_string -- a Diamond
 * String is already a raw byte buffer, not UTF-8-validated, so a blob's
 * raw bytes need no separate representation. A zero-length text/blob's
 * native pointer can be nullptr; allocate_string's memcpy(dest,NULL,0)
 * would be UB even though every real implementation treats it as a
 * no-op, so it's substituted with "" rather than relying on that. */
static bool sqlite3_column_value_helper(DiamondVm *vm,sqlite3_stmt *stmt,int column,
        DiamondValue *out) {
    switch(sqlite3_column_type(stmt,column)) {
        case SQLITE_INTEGER:
            *out=DIAMOND_INT(sqlite3_column_int64(stmt,column));return true;
        case SQLITE_FLOAT:
            *out=DIAMOND_FLOAT(sqlite3_column_double(stmt,column));return true;
        case SQLITE_NULL:
            *out=DIAMOND_NIL;return true;
        case SQLITE_TEXT: {
            const char *text=(const char *)sqlite3_column_text(stmt,column);
            const size_t length=(size_t)sqlite3_column_bytes(stmt,column);
            DiamondString *string=allocate_string(vm,text!=nullptr?text:"",length);
            if(string==nullptr)return false;
            *out=DIAMOND_OBJECT(string);return true;
        }
        case SQLITE_BLOB: {
            const char *bytes=(const char *)sqlite3_column_blob(stmt,column);
            const size_t length=(size_t)sqlite3_column_bytes(stmt,column);
            DiamondString *string=allocate_string(vm,bytes!=nullptr?bytes:"",length);
            if(string==nullptr)return false;
            *out=DIAMOND_OBJECT(string);return true;
        }
        default:
            *out=DIAMOND_NIL;return true;
    }
}

/* Unlike every other helper in this file (regexp_new_helper,
 * call_closure_helper, ...), `result` here MUST be a pointer into the
 * caller's live `registers` array (i.e. the caller passes
 * &registers[dest] directly, not an intermediate local later copied
 * in) -- this helper allocates repeatedly (one Hash per row, one String
 * per key and per Text/Blob column) while building the result, and only
 * an allocation already reachable from a genuine GC root survives a
 * collection triggered by a *later* allocation in that same sequence.
 * `*result` is written to hold the outer Array immediately, before any
 * further allocation, exactly like DIAMOND_OP_IO_POLL/UDPSocket#receive's
 * own Hash result (see their shared comment) -- and every row Hash is
 * pushed onto that already-rooted Array *before* its own columns are
 * filled in (array_push never itself allocates a Diamond object, only
 * reallocates the Array's own backing store via plain realloc, so this
 * costs nothing), so a row is reachable through the rooted Array for the
 * entire time its columns are being built up one allocation at a time. */
static DiamondVmStatus sqlite3_collect_rows_helper(DiamondVm *vm,sqlite3 *db,
        sqlite3_stmt *stmt,DiamondValue *result) {
    DiamondArray *rows=allocate_array(vm,nullptr,0);
    if(rows==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *result=DIAMOND_OBJECT(rows);
    const int column_count=sqlite3_column_count(stmt);
    for(;;) {
        const int rc=sqlite3_step(stmt);
        if(rc==SQLITE_DONE)break;
        if(rc!=SQLITE_ROW) {
            snprintf(vm->error,sizeof vm->error,"%s",sqlite3_errmsg(db));
            return DIAMOND_VM_SQLITE3_ERROR;
        }
        DiamondHash *row=allocate_hash(vm);
        if(row==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        if(!array_push(vm,rows,DIAMOND_OBJECT(row)))return DIAMOND_VM_OUT_OF_MEMORY;
        for(int column=0;column<column_count;column++) {
            const char *column_name=sqlite3_column_name(stmt,column);
            DiamondString *key=allocate_string(vm,column_name,strlen(column_name));
            if(key==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
            if(!hash_set(vm,row,DIAMOND_OBJECT(key),DIAMOND_NIL))return DIAMOND_VM_OUT_OF_MEMORY;
            DiamondValue column_value=DIAMOND_NIL;
            if(!sqlite3_column_value_helper(vm,stmt,column,&column_value))
                return DIAMOND_VM_OUT_OF_MEMORY;
            if(!hash_set(vm,row,DIAMOND_OBJECT(key),column_value))
                return DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    return DIAMOND_VM_OK;
}

/* Unlike every other helper in this file (regexp_new_helper,
 * call_closure_helper, ...), `result` here MUST be a pointer into the
 * caller's live `registers` array (i.e. the caller passes
 * &registers[dest] directly, not an intermediate local later copied
 * in) -- sqlite3_collect_rows_helper allocates repeatedly (one Hash per
 * row, one String per key and per Text/Blob column) while building the
 * result, and only an allocation already reachable from a genuine GC
 * root survives a collection triggered by a *later* allocation in that
 * same sequence. `*result` is written to hold the outer Array
 * immediately, before any further allocation, exactly like
 * DIAMOND_OP_IO_POLL/UDPSocket#receive's own Hash result (see their
 * shared comment) -- and every row Hash is pushed onto that already-
 * rooted Array *before* its own columns are filled in (array_push never
 * itself allocates a Diamond object, only reallocates the Array's own
 * backing store via plain realloc, so this costs nothing), so a row is
 * reachable through the rooted Array for the entire time its columns are
 * being built up one allocation at a time. */
static DiamondVmStatus sqlite3_query_helper(DiamondVm *vm,DiamondSqlite3Handle *handle,
        const DiamondString *sql,DiamondValue params,DiamondValue *result) {
    sqlite3_stmt *stmt=nullptr;
    const DiamondVmStatus prepare_status=
        sqlite3_prepare_helper(vm,handle->db,sql,params,&stmt);
    if(prepare_status!=DIAMOND_VM_OK)return prepare_status;
    const DiamondVmStatus collect_status=
        sqlite3_collect_rows_helper(vm,handle->db,stmt,result);
    sqlite3_finalize(stmt);
    return collect_status;
}

/* Statement#execute/#query share the same reset-and-rebind step: unlike
 * the one-shot #execute/#query path, `stmt` is already prepared and must
 * survive this call for reuse, so it's never finalized here -- only
 * Statement#close and GC/VM teardown finalize it. sqlite3_reset rewinds
 * the statement back to its first sqlite3_step-able state (required
 * before rebinding -- sqlite3_bind_* on a statement mid-execution is
 * itself an error) and sqlite3_clear_bindings drops any bind values
 * from a previous call so an omitted/absent bind on this call can't
 * accidentally reuse a stale value from the one before it. */
static DiamondVmStatus sqlite3_statement_bind_and_reset_helper(DiamondVm *vm,
        sqlite3_stmt *stmt,DiamondValue params) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return sqlite3_bind_helper(vm,stmt,params);
}

static DiamondVmStatus sqlite3_statement_execute_helper(DiamondVm *vm,sqlite3 *db,
        sqlite3_stmt *stmt,DiamondValue params,DiamondValue *result) {
    const DiamondVmStatus bind_status=sqlite3_statement_bind_and_reset_helper(vm,stmt,params);
    if(bind_status!=DIAMOND_VM_OK)return bind_status;
    return sqlite3_run_to_completion_helper(vm,db,stmt,result);
}

/* Same &registers[dest]-must-be-a-real-GC-root requirement as
 * sqlite3_query_helper above -- see its own comment. */
static DiamondVmStatus sqlite3_statement_query_helper(DiamondVm *vm,sqlite3 *db,
        sqlite3_stmt *stmt,DiamondValue params,DiamondValue *result) {
    const DiamondVmStatus bind_status=sqlite3_statement_bind_and_reset_helper(vm,stmt,params);
    if(bind_status!=DIAMOND_VM_OK)return bind_status;
    return sqlite3_collect_rows_helper(vm,db,stmt,result);
}

/* All of #execute/#query/#prepare/#last_insert_row_id/#close's method-name
 * comparison and argument marshaling, factored out of the INVOKE case
 * body for the same reason get_cvar_helper/call_closure_helper/
 * regexp_new_helper already are: every local declared anywhere in
 * run_chunk's own switch adds to its one shared per-call stack frame at
 * -O0 regardless of which case actually runs (run_chunk recurses in C
 * for every Diamond-level call), and this dispatch alone -- five method-
 * name bools, a handle pointer, a sql pointer, a params value --
 * was enough on its own to reopen the exact DIAMOND_MAX_CALL_DEPTH/ASan
 * margin regression documented elsewhere in this codebase (confirmed the
 * hard way: `depth(5000)` overflowed the real C stack under
 * -fsanitize=address before this was pulled out). `registers`/`base`/
 * `dest` are passed through unchanged, so sqlite3_query_helper's own
 * &registers[dest] GC-rooting still targets the real per-frame register
 * array either way -- moving this dispatch into its own function changes
 * nothing about which array that pointer refers to. */
DiamondVmStatus sqlite3_dispatch_helper(DiamondVm *vm,DiamondSqlite3Handle *target_db,
        const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base,
        uint8_t argc,uint16_t dest) {
    const bool execute_method=method_name->length==7&&
        memcmp(method_name->chars,"execute",7)==0;
    const bool query_method=method_name->length==5&&
        memcmp(method_name->chars,"query",5)==0;
    const bool prepare_method=method_name->length==7&&
        memcmp(method_name->chars,"prepare",7)==0;
    const bool last_insert_row_id_method=method_name->length==18&&
        memcmp(method_name->chars,"last_insert_row_id",18)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    if(!execute_method&&!query_method&&!prepare_method&&
       !last_insert_row_id_method&&!close_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"SQLite3");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(target_db->db!=nullptr) {
            /* _v2 -- see diamond_vm_collect_impl's matching SQLITE3 branch
             * for why plain sqlite3_close is unsafe once Statements exist. */
            sqlite3_close_v2(target_db->db);
            target_db->db=nullptr;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(target_db->db==nullptr) {
        snprintf(vm->error,sizeof vm->error,"SQLite3 connection is closed");
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    if(last_insert_row_id_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_INT(sqlite3_last_insert_rowid(target_db->db));
        return DIAMOND_VM_OK;
    }
    if(prepare_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,"SQLite3#prepare's sql argument must be a String");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *sql=(const DiamondString *)registers[base].as.object;
        sqlite3_stmt *stmt=nullptr;
        const DiamondVmStatus prepare_status=
            sqlite3_prepare_only_helper(vm,target_db->db,sql,&stmt);
        if(prepare_status!=DIAMOND_VM_OK)return prepare_status;
        DiamondSqlite3StatementHandle *handle=allocate_sqlite3_statement_handle(vm,stmt);
        if(handle==nullptr) {
            sqlite3_finalize(stmt);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        registers[dest]=DIAMOND_OBJECT(handle);
        return DIAMOND_VM_OK;
    }
    /* execute/query share the same argument shape: (sql) or (sql, params). */
    if(argc!=1&&argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"SQLite3#%.*s's sql argument must be a String",
            (int)method_name->length,method_name->chars);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *sql=(const DiamondString *)registers[base].as.object;
    const DiamondValue params=argc==2?registers[(size_t)base+1]:DIAMOND_NIL;
    if(execute_method) {
        DiamondValue execute_result=DIAMOND_NIL;
        const DiamondVmStatus execute_status=sqlite3_execute_helper(vm,
            target_db,sql,params,&execute_result);
        if(execute_status!=DIAMOND_VM_OK)return execute_status;
        registers[dest]=execute_result;return DIAMOND_VM_OK;
    }
    /* query_method: sqlite3_query_helper writes directly into
     * registers[dest] (a real GC root), not a local -- see its own
     * comment. */
    return sqlite3_query_helper(vm,target_db,sql,params,&registers[dest]);
}

/* Statement#execute/#query/#close -- same three-way dispatch shape as
 * SQLite3 itself, minus #prepare (a Statement isn't a connection) and
 * #last_insert_row_id (still only meaningful on the connection). `params`
 * is accepted the same way (Nil/Array/Hash) as the connection's own
 * #execute/#query. */
DiamondVmStatus sqlite3_statement_dispatch_helper(DiamondVm *vm,
        DiamondSqlite3StatementHandle *target_stmt,const DiamondStringConstant *method_name,
        DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest) {
    const bool execute_method=method_name->length==7&&
        memcmp(method_name->chars,"execute",7)==0;
    const bool query_method=method_name->length==5&&
        memcmp(method_name->chars,"query",5)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    if(!execute_method&&!query_method&&!close_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"SQLite3::Statement");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(target_stmt->stmt!=nullptr) {
            sqlite3_finalize(target_stmt->stmt);
            target_stmt->stmt=nullptr;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(target_stmt->stmt==nullptr) {
        snprintf(vm->error,sizeof vm->error,"SQLite3::Statement is closed");
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    /* execute/query share the same argument shape: () or (params). */
    if(argc!=0&&argc!=1)return DIAMOND_VM_ARITY_ERROR;
    const DiamondValue params=argc==1?registers[base]:DIAMOND_NIL;
    sqlite3 *const db=sqlite3_db_handle(target_stmt->stmt);
    if(execute_method) {
        DiamondValue execute_result=DIAMOND_NIL;
        const DiamondVmStatus execute_status=sqlite3_statement_execute_helper(vm,
            db,target_stmt->stmt,params,&execute_result);
        if(execute_status!=DIAMOND_VM_OK)return execute_status;
        registers[dest]=execute_result;return DIAMOND_VM_OK;
    }
    /* query_method: sqlite3_statement_query_helper writes directly into
     * registers[dest] (a real GC root), not a local -- see
     * sqlite3_query_helper's own comment. */
    return sqlite3_statement_query_helper(vm,db,target_stmt->stmt,params,&registers[dest]);
}

/* Behind DIAMOND_OP_SQLITE3_OPEN. `mode` is Nil (mode omitted -- falls
 * back to plain sqlite3_open unchanged, exactly the pre-existing
 * behavior, rather than routing the common no-mode-given call through
 * _v2 for no behavioral reason) or a String: "r" (read-only, error if
 * missing), "rw" (read-write, error if missing), or "rwc" (read-write,
 * created if missing -- sqlite3_open's own default, spelled out
 * explicitly). Anything else is a Diamond-level call-shape mistake,
 * TypeError, checked here (runtime) rather than at parse time since
 * `mode` is an arbitrary expression, not a literal. */
DiamondVmStatus sqlite3_open_helper(DiamondVm *vm,const DiamondString *path,
        DiamondValue mode,sqlite3 **out_db) {
    sqlite3 *db=nullptr;
    int open_rc;
    if(mode.kind==DIAMOND_VALUE_NIL) {
        open_rc=sqlite3_open(path->chars,&db);
    } else {
        if(mode.kind!=DIAMOND_VALUE_OBJECT||mode.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,"SQLite3.open's mode argument must be a String");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *mode_string=(const DiamondString *)mode.as.object;
        int flags;
        if(mode_string->length==1&&mode_string->chars[0]=='r') {
            flags=SQLITE_OPEN_READONLY;
        } else if(mode_string->length==2&&memcmp(mode_string->chars,"rw",2)==0) {
            flags=SQLITE_OPEN_READWRITE;
        } else if(mode_string->length==3&&memcmp(mode_string->chars,"rwc",3)==0) {
            flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE;
        } else {
            snprintf(vm->error,sizeof vm->error,
                "SQLite3.open's mode must be \"r\", \"rw\", or \"rwc\", got \"%.*s\"",
                (int)mode_string->length,mode_string->chars);
            return DIAMOND_VM_TYPE_ERROR;
        }
        open_rc=sqlite3_open_v2(path->chars,&db,flags,nullptr);
    }
    if(open_rc!=SQLITE_OK) {
        snprintf(vm->error,sizeof vm->error,"cannot open '%.*s': %s",
                 (int)path->length,path->chars,sqlite3_errmsg(db));
        sqlite3_close(db);
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    *out_db=db;
    return DIAMOND_VM_OK;
}

/* Postgres OIDs for the column types this driver decodes into a native
 * Diamond type below. libpq-devel doesn't ship pg_type.h (that lives in
 * postgresql-server-devel, a separate package this driver doesn't
 * require) -- these are Postgres's own well-known, long-stable builtin
 * type OIDs, hardcoded directly rather than pulling in a whole extra
 * dependency for ten integer constants. Anything else (date/timestamp/
 * json/jsonb/uuid/bytea/arrays/...) decodes as the raw text libpq
 * already returns -- an explicit, documented scope cut, not silent data
 * loss (bytea in particular stays Postgres's default hex-text spelling,
 * not raw bytes). */
#define DIAMOND_PG_BOOLOID 16
#define DIAMOND_PG_INT8OID 20
#define DIAMOND_PG_INT2OID 21
#define DIAMOND_PG_INT4OID 23
#define DIAMOND_PG_FLOAT4OID 700
#define DIAMOND_PG_FLOAT8OID 701
#define DIAMOND_PG_NUMERICOID 1700

/* `?` -> `$1`/`$2`/... translation: SQLite3's own placeholder spelling is
 * kept at the Diamond level for API/Arel-adapter consistency even though
 * libpq's PQexecParams requires numbered placeholders. A `?` inside a
 * single-quoted string literal (`''` is the standard SQL escaped quote)
 * is left alone; nothing else is -- a literal `?` used outside a string
 * (Postgres's own JSONB "key exists" operator, for instance) isn't
 * distinguishable from a placeholder here and isn't supported through
 * the params-array call form in this first slice. Two-pass (count
 * placeholders, then allocate exactly and fill) rather than repeated
 * reallocation, the same one-allocation stance the rest of this codebase
 * already takes for string building. Returns nullptr only on OOM. */
static char *postgres_translate_placeholders_helper(const char *sql,size_t length,
        size_t *out_count) {
    size_t placeholder_count=0;
    bool in_string=false;
    for(size_t index=0;index<length;index++) {
        const char ch=sql[index];
        if(in_string) {
            if(ch=='\'') {
                if(index+1<length&&sql[index+1]=='\'')index++;
                else in_string=false;
            }
        } else if(ch=='\'') {
            in_string=true;
        } else if(ch=='?') {
            placeholder_count++;
        }
    }
    /* Each `?` (1 char) becomes `$` plus up to 10 digits -- far more
     * headroom than any realistic placeholder count needs, sized once. */
    const size_t max_extra_per_placeholder=10;
    char *out=malloc(length+placeholder_count*max_extra_per_placeholder+1);
    if(out==nullptr)return nullptr;
    size_t out_index=0,placeholder_number=0;
    in_string=false;
    for(size_t index=0;index<length;index++) {
        const char ch=sql[index];
        if(in_string) {
            out[out_index++]=ch;
            if(ch=='\'') {
                if(index+1<length&&sql[index+1]=='\'')out[out_index++]=sql[++index];
                else in_string=false;
            }
        } else if(ch=='\'') {
            in_string=true;out[out_index++]=ch;
        } else if(ch=='?') {
            placeholder_number++;
            out_index+=(size_t)snprintf(out+out_index,max_extra_per_placeholder+1,
                "$%zu",placeholder_number);
        } else {
            out[out_index++]=ch;
        }
    }
    out[out_index]='\0';
    *out_count=placeholder_count;
    return out;
}

static void postgres_free_params_helper(char **param_values,size_t count) {
    if(param_values==nullptr)return;
    for(size_t index=0;index<count;index++)free(param_values[index]);
    free(param_values);
}

/* Converts an Array of Diamond values into a NUL-terminated C string
 * array for PQexecParams's text-format parameter list (a Nil entry stays
 * a null pointer, PQexecParams's own spelling of SQL NULL); the caller
 * frees it via postgres_free_params_helper. Deliberately mirrors
 * sqlite3_bind_params_helper's exact same type-support boundary (Nil/
 * Int/Float/Bool/String only, no Array/Hash/Instance/bignum-promoted
 * Int) for consistency between the two drivers. */
static DiamondVmStatus postgres_bind_params_helper(DiamondVm *vm,
        const DiamondValue *values,size_t count,char ***out_values) {
    char **param_values=count==0?nullptr:calloc(count,sizeof(char *));
    if(count>0&&param_values==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    for(size_t index=0;index<count;index++) {
        const DiamondValue value=values[index];
        if(value.kind==DIAMOND_VALUE_NIL)continue; /* stays nullptr = SQL NULL */
        char buffer[64];
        if(value.kind==DIAMOND_VALUE_INT) {
            snprintf(buffer,sizeof buffer,"%" PRId64,value.as.integer);
        } else if(value.kind==DIAMOND_VALUE_FLOAT) {
            if(isnan(value.as.real))snprintf(buffer,sizeof buffer,"NaN");
            else if(isinf(value.as.real))
                snprintf(buffer,sizeof buffer,"%s",value.as.real<0?"-Infinity":"Infinity");
            else snprintf(buffer,sizeof buffer,"%.17g",value.as.real);
        } else if(value.kind==DIAMOND_VALUE_BOOL) {
            snprintf(buffer,sizeof buffer,"%s",value.as.boolean?"true":"false");
        } else if(value.kind==DIAMOND_VALUE_OBJECT&&
                  value.as.object->kind==DIAMOND_OBJECT_STRING) {
            const DiamondString *string=(const DiamondString *)value.as.object;
            char *copy=malloc(string->length+1);
            if(copy==nullptr) {
                postgres_free_params_helper(param_values,count);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            memcpy(copy,string->chars,string->length);
            copy[string->length]='\0';
            param_values[index]=copy;
            continue;
        } else {
            snprintf(vm->error,sizeof vm->error,
                "unsupported PostgreSQL parameter type at position %zu",index+1);
            postgres_free_params_helper(param_values,count);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const size_t buffer_length=strlen(buffer);
        char *copy=malloc(buffer_length+1);
        if(copy==nullptr) {
            postgres_free_params_helper(param_values,count);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        memcpy(copy,buffer,buffer_length+1);
        param_values[index]=copy;
    }
    *out_values=param_values;
    return DIAMOND_VM_OK;
}

/* PQgetvalue's text-format output decoded to the matching native Diamond
 * type for the well-known OIDs above; anything else stays the raw String
 * libpq already returns. Only the String-allocation path can fail (OOM),
 * matching sqlite3_column_value_helper's own bool-return/out-param shape.
 * INT2/INT4/INT8OID are all guaranteed to fit int64_t by Postgres's own
 * type bounds, so unlike String#to_i there's no bignum-overflow case to
 * handle here; NUMERIC has no such bound but is deliberately decoded as
 * Float (lossy for values outside double precision), not Int. */
static bool postgres_decode_value_helper(DiamondVm *vm,PGresult *res,int row,int col,
        DiamondValue *out) {
    if(PQgetisnull(res,row,col)) {
        *out=DIAMOND_NIL;return true;
    }
    const char *text=PQgetvalue(res,row,col);
    switch(PQftype(res,col)) {
        case DIAMOND_PG_BOOLOID:
            *out=DIAMOND_BOOL(text[0]=='t');return true;
        case DIAMOND_PG_INT2OID:
        case DIAMOND_PG_INT4OID:
        case DIAMOND_PG_INT8OID:
            *out=DIAMOND_INT(strtoll(text,nullptr,10));return true;
        case DIAMOND_PG_FLOAT4OID:
        case DIAMOND_PG_FLOAT8OID:
        case DIAMOND_PG_NUMERICOID:
            *out=DIAMOND_FLOAT(strtod(text,nullptr));return true;
        default: {
            const size_t length=(size_t)PQgetlength(res,row,col);
            DiamondString *string=allocate_string(vm,text,length);
            if(string==nullptr)return false;
            *out=DIAMOND_OBJECT(string);return true;
        }
    }
}

/* Shared translate+bind+exec step behind #execute/#query/the internal
 * SELECT lastval() #last_insert_row_id() runs. Unlike
 * sqlite3_prepare_helper, no manual multiple-statements guard is needed:
 * PQexecParams itself refuses more than one SQL command regardless of
 * parameter count, surfacing that as an ordinary error PGresult this
 * function already turns into DIAMOND_VM_POSTGRES_ERROR. */
static DiamondVmStatus postgres_exec_helper(DiamondVm *vm,DiamondPostgresHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,PGresult **out_res) {
    size_t placeholder_count=0;
    char *translated=postgres_translate_placeholders_helper(sql_chars,sql_length,
        &placeholder_count);
    if(translated==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(param_count!=placeholder_count) {
        free(translated);
        snprintf(vm->error,sizeof vm->error,
            "PostgreSQL statement expects %zu bound parameter(s), got %zu",
            placeholder_count,param_count);
        return DIAMOND_VM_ARITY_ERROR;
    }
    char **bound_values=nullptr;
    const DiamondVmStatus bind_status=
        postgres_bind_params_helper(vm,param_values,param_count,&bound_values);
    if(bind_status!=DIAMOND_VM_OK) {
        free(translated);
        return bind_status;
    }
    PGresult *res=PQexecParams(handle->conn,translated,(int)param_count,nullptr,
        (const char *const *)bound_values,nullptr,nullptr,0);
    free(translated);
    postgres_free_params_helper(bound_values,param_count);
    if(res==nullptr) {
        snprintf(vm->error,sizeof vm->error,"%s",PQerrorMessage(handle->conn));
        return DIAMOND_VM_POSTGRES_ERROR;
    }
    const ExecStatusType status=PQresultStatus(res);
    if(status!=PGRES_TUPLES_OK&&status!=PGRES_COMMAND_OK) {
        snprintf(vm->error,sizeof vm->error,"%s",PQresultErrorMessage(res));
        PQclear(res);
        return DIAMOND_VM_POSTGRES_ERROR;
    }
    *out_res=res;
    return DIAMOND_VM_OK;
}

static DiamondVmStatus postgres_execute_helper(DiamondVm *vm,DiamondPostgresHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,DiamondValue *result) {
    PGresult *res=nullptr;
    const DiamondVmStatus status=
        postgres_exec_helper(vm,handle,sql_chars,sql_length,param_values,param_count,&res);
    if(status!=DIAMOND_VM_OK)return status;
    const char *affected=PQcmdTuples(res);
    *result=DIAMOND_INT(affected[0]=='\0'?0:strtoll(affected,nullptr,10));
    PQclear(res);
    return DIAMOND_VM_OK;
}

/* Same GC-rooting discipline as sqlite3_query_helper: `result` must be a
 * pointer into the caller's live registers array, `*result` is written
 * to hold the outer Array before any further allocation, and each row's
 * Hash is pushed onto that already-rooted Array (then each key set to
 * Nil first, rooting the key before decoding may itself allocate a
 * String) before its real column values are filled in one at a time. */
static DiamondVmStatus postgres_query_helper(DiamondVm *vm,DiamondPostgresHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,DiamondValue *result) {
    PGresult *res=nullptr;
    const DiamondVmStatus status=
        postgres_exec_helper(vm,handle,sql_chars,sql_length,param_values,param_count,&res);
    if(status!=DIAMOND_VM_OK)return status;
    DiamondArray *rows=allocate_array(vm,nullptr,0);
    if(rows==nullptr) {
        PQclear(res);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *result=DIAMOND_OBJECT(rows);
    const int row_count=PQntuples(res);
    const int column_count=PQnfields(res);
    for(int row=0;row<row_count;row++) {
        DiamondHash *row_hash=allocate_hash(vm);
        if(row_hash==nullptr) {
            PQclear(res);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        if(!array_push(vm,rows,DIAMOND_OBJECT(row_hash))) {
            PQclear(res);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        for(int col=0;col<column_count;col++) {
            const char *column_name=PQfname(res,col);
            DiamondString *key=allocate_string(vm,column_name,strlen(column_name));
            if(key==nullptr) {
                PQclear(res);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            if(!hash_set(vm,row_hash,DIAMOND_OBJECT(key),DIAMOND_NIL)) {
                PQclear(res);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            DiamondValue column_value=DIAMOND_NIL;
            if(!postgres_decode_value_helper(vm,res,row,col,&column_value)) {
                PQclear(res);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            if(!hash_set(vm,row_hash,DIAMOND_OBJECT(key),column_value)) {
                PQclear(res);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
        }
    }
    PQclear(res);
    return DIAMOND_VM_OK;
}

/* #execute/#query/#last_insert_row_id/#close -- factored out of the
 * INVOKE case body for the same stack-frame reason sqlite3_dispatch_
 * helper's own comment explains. #last_insert_row_id runs `SELECT
 * lastval()` through postgres_query_helper and unwraps the single Int
 * cell; it fails with a real PostgreSQLError (lastval's own "not yet
 * defined in this session" condition) if no sequence has been used yet
 * on this connection, the same honest-failure spirit as everything else
 * this driver raises. */
DiamondVmStatus postgres_dispatch_helper(DiamondVm *vm,DiamondPostgresHandle *target_db,
        const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base,
        uint8_t argc,uint16_t dest) {
    const bool execute_method=method_name->length==7&&
        memcmp(method_name->chars,"execute",7)==0;
    const bool query_method=method_name->length==5&&
        memcmp(method_name->chars,"query",5)==0;
    const bool last_insert_row_id_method=method_name->length==18&&
        memcmp(method_name->chars,"last_insert_row_id",18)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    if(!execute_method&&!query_method&&!last_insert_row_id_method&&!close_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"PostgreSQL");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(target_db->conn!=nullptr) {
            PQfinish(target_db->conn);
            target_db->conn=nullptr;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(target_db->conn==nullptr) {
        snprintf(vm->error,sizeof vm->error,"PostgreSQL connection is closed");
        return DIAMOND_VM_POSTGRES_ERROR;
    }
    if(last_insert_row_id_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        static const char lastval_sql[]="SELECT lastval()";
        DiamondValue rows=DIAMOND_NIL;
        const DiamondVmStatus query_status=postgres_query_helper(vm,target_db,
            lastval_sql,sizeof lastval_sql-1,nullptr,0,&rows);
        if(query_status!=DIAMOND_VM_OK)return query_status;
        const DiamondArray *row_array=(const DiamondArray *)rows.as.object;
        const DiamondHash *row=(const DiamondHash *)row_array->values[0].as.object;
        registers[dest]=row->entries[0].value;
        return DIAMOND_VM_OK;
    }
    /* execute/query share the same argument shape: (sql) or (sql, params). */
    if(argc!=1&&argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"PostgreSQL#%.*s's sql argument must be a String",
            (int)method_name->length,method_name->chars);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *sql=(const DiamondString *)registers[base].as.object;
    const DiamondValue *param_values=nullptr;
    size_t param_count=0;
    if(argc==2) {
        if(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
            snprintf(vm->error,sizeof vm->error,
                "PostgreSQL#%.*s's params argument must be an Array",
                (int)method_name->length,method_name->chars);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondArray *params=(const DiamondArray *)registers[(size_t)base+1].as.object;
        param_values=params->values;
        param_count=params->count;
    }
    if(execute_method) {
        DiamondValue execute_result=DIAMOND_NIL;
        const DiamondVmStatus execute_status=postgres_execute_helper(vm,
            target_db,sql->chars,sql->length,param_values,param_count,&execute_result);
        if(execute_status!=DIAMOND_VM_OK)return execute_status;
        registers[dest]=execute_result;return DIAMOND_VM_OK;
    }
    /* query_method: postgres_query_helper writes directly into
     * registers[dest] (a real GC root), not a local -- see its own
     * comment. */
    return postgres_query_helper(vm,target_db,sql->chars,sql->length,param_values,
        param_count,&registers[dest]);
}

/* Fixed-width storage for a bound parameter's native value -- unlike
 * postgres_bind_params_helper (which must copy into freshly malloc'd,
 * NUL-terminated C strings for PQexecParams's text-format protocol),
 * MYSQL_BIND takes an explicit buffer_length for every type, so a String
 * parameter's buffer can point directly at the DiamondString's own
 * `chars` (safe here specifically because nothing between building this
 * array and mysql_stmt_execute consuming it can allocate and trigger a
 * GC pass -- the DiamondString stays reachable from the live `registers`
 * root throughout). Only Int/Float/Bool need an addressable copy of
 * their own, since DiamondValue's own layout isn't what MYSQL_BIND wants
 * pointed at. */
typedef union DiamondMysqlParamStorage {
    int64_t as_int64;
    double as_double;
    signed char as_tiny;
} DiamondMysqlParamStorage;

enum { DIAMOND_MYSQL_FIXED_FIELD_WIDTH = 128 };

/* Builds a MYSQL_BIND array (plus the storage array backing its non-String
 * buffers) for mysql_stmt_bind_param -- mirrors postgres_bind_params_helper's
 * exact same type-support boundary (Nil/Int/Float/Bool/String only) for
 * consistency between the two drivers. Caller frees both arrays once
 * mysql_stmt_execute has consumed them. */
static DiamondVmStatus mysql_bind_params_helper(DiamondVm *vm,
        const DiamondValue *values,size_t count,MYSQL_BIND **out_binds,
        DiamondMysqlParamStorage **out_storage) {
    MYSQL_BIND *binds=count==0?nullptr:calloc(count,sizeof(MYSQL_BIND));
    DiamondMysqlParamStorage *storage=
        count==0?nullptr:calloc(count,sizeof(DiamondMysqlParamStorage));
    if(count>0&&(binds==nullptr||storage==nullptr)) {
        free(binds);free(storage);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    for(size_t index=0;index<count;index++) {
        const DiamondValue value=values[index];
        if(value.kind==DIAMOND_VALUE_NIL) {
            binds[index].buffer_type=MYSQL_TYPE_NULL;
        } else if(value.kind==DIAMOND_VALUE_INT) {
            storage[index].as_int64=value.as.integer;
            binds[index].buffer_type=MYSQL_TYPE_LONGLONG;
            binds[index].buffer=&storage[index].as_int64;
        } else if(value.kind==DIAMOND_VALUE_FLOAT) {
            storage[index].as_double=value.as.real;
            binds[index].buffer_type=MYSQL_TYPE_DOUBLE;
            binds[index].buffer=&storage[index].as_double;
        } else if(value.kind==DIAMOND_VALUE_BOOL) {
            storage[index].as_tiny=value.as.boolean?1:0;
            binds[index].buffer_type=MYSQL_TYPE_TINY;
            binds[index].buffer=&storage[index].as_tiny;
        } else if(value.kind==DIAMOND_VALUE_OBJECT&&
                  value.as.object->kind==DIAMOND_OBJECT_STRING) {
            const DiamondString *string=(const DiamondString *)value.as.object;
            binds[index].buffer_type=MYSQL_TYPE_STRING;
            binds[index].buffer=(void *)string->chars;
            binds[index].buffer_length=(unsigned long)string->length;
        } else {
            snprintf(vm->error,sizeof vm->error,
                "unsupported MySQL parameter type at position %zu",index+1);
            free(binds);free(storage);
            return DIAMOND_VM_TYPE_ERROR;
        }
    }
    *out_binds=binds;
    *out_storage=storage;
    return DIAMOND_VM_OK;
}

/* PQftype's role for this driver: the field metadata type this column's
 * fetched string bytes should decode into. Every output column is bound
 * as MYSQL_TYPE_STRING (see mysql_query_helper) so the server itself
 * always hands back a text representation regardless of the column's real
 * wire type -- fields[col].type (captured before that rebinding) is the
 * only place the original type survives. Same documented scope cut as
 * postgres_decode_value_helper: anything not a recognized integer or
 * floating type stays the raw String already fetched (dates/times/blobs/
 * json included). MySQL has no native boolean type -- TINYINT(1) is only
 * a convention, indistinguishable at the protocol level from any other
 * TINYINT -- so unlike Postgres's real BOOLOID, every integer type here
 * decodes as Int, matching sqlite3_column_value_helper's same tradeoff. */
static bool mysql_decode_value_helper(DiamondVm *vm,const MYSQL_FIELD *field,
        const char *bytes,unsigned long length,DiamondValue *out) {
    switch(field->type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR: {
            char buffer[32];
            const size_t copy_length=length<sizeof buffer-1?length:sizeof buffer-1;
            memcpy(buffer,bytes,copy_length);buffer[copy_length]='\0';
            *out=DIAMOND_INT(strtoll(buffer,nullptr,10));
            return true;
        }
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL: {
            char buffer[64];
            const size_t copy_length=length<sizeof buffer-1?length:sizeof buffer-1;
            memcpy(buffer,bytes,copy_length);buffer[copy_length]='\0';
            *out=DIAMOND_FLOAT(strtod(buffer,nullptr));
            return true;
        }
        default: {
            DiamondString *string=allocate_string(vm,bytes,(size_t)length);
            if(string==nullptr)return false;
            *out=DIAMOND_OBJECT(string);
            return true;
        }
    }
}

/* Shared prepare+bind+execute step behind #execute/#query. Unlike
 * postgres_exec_helper, no `?` -> `$N` placeholder translation is needed
 * -- MySQL's own prepared-statement placeholder spelling already is `?`.
 * mysql_stmt_param_count(stmt) after a successful prepare gives an exact
 * placeholder count to validate the caller's params Array against, the
 * same arity check postgres_exec_helper derives from its own translation
 * pass. No manual multiple-statement guard is needed either: this
 * connection is never opened with CLIENT_MULTI_STATEMENTS (see
 * DIAMOND_OP_MYSQL_OPEN), so a semicolon-separated second statement is
 * simply a syntax error from mysql_stmt_prepare itself. */
static DiamondVmStatus mysql_exec_helper(DiamondVm *vm,DiamondMysqlHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,MYSQL_STMT **out_stmt) {
    MYSQL_STMT *stmt=mysql_stmt_init(handle->conn);
    if(stmt==nullptr) {
        snprintf(vm->error,sizeof vm->error,"%s",mysql_error(handle->conn));
        return DIAMOND_VM_MYSQL_ERROR;
    }
    if(mysql_stmt_prepare(stmt,sql_chars,(unsigned long)sql_length)!=0) {
        snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return DIAMOND_VM_MYSQL_ERROR;
    }
    const unsigned long placeholder_count=mysql_stmt_param_count(stmt);
    if((unsigned long)param_count!=placeholder_count) {
        snprintf(vm->error,sizeof vm->error,
            "MySQL statement expects %lu bound parameter(s), got %zu",
            placeholder_count,param_count);
        mysql_stmt_close(stmt);
        return DIAMOND_VM_ARITY_ERROR;
    }
    if(param_count>0) {
        MYSQL_BIND *binds=nullptr;
        DiamondMysqlParamStorage *storage=nullptr;
        const DiamondVmStatus bind_status=
            mysql_bind_params_helper(vm,param_values,param_count,&binds,&storage);
        if(bind_status!=DIAMOND_VM_OK) {
            mysql_stmt_close(stmt);
            return bind_status;
        }
        if(mysql_stmt_bind_param(stmt,binds)!=0) {
            snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
            free(binds);free(storage);
            mysql_stmt_close(stmt);
            return DIAMOND_VM_MYSQL_ERROR;
        }
        const int execute_result=mysql_stmt_execute(stmt);
        free(binds);free(storage);
        if(execute_result!=0) {
            snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            return DIAMOND_VM_MYSQL_ERROR;
        }
    } else if(mysql_stmt_execute(stmt)!=0) {
        snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return DIAMOND_VM_MYSQL_ERROR;
    }
    *out_stmt=stmt;
    return DIAMOND_VM_OK;
}

static DiamondVmStatus mysql_execute_helper(DiamondVm *vm,DiamondMysqlHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,DiamondValue *result) {
    MYSQL_STMT *stmt=nullptr;
    const DiamondVmStatus status=mysql_exec_helper(vm,handle,sql_chars,sql_length,
        param_values,param_count,&stmt);
    if(status!=DIAMOND_VM_OK)return status;
    *result=DIAMOND_INT((int64_t)mysql_stmt_affected_rows(stmt));
    mysql_stmt_close(stmt);
    return DIAMOND_VM_OK;
}

/* Fixed-width result types this driver gives a small inline buffer
 * directly in the one mysql_stmt_bind_result call, rather than the
 * null-buffer-probe-then-mysql_stmt_fetch_column two-phase dance query
 * helper below uses for genuinely unbounded types (STRING/VAR_STRING/
 * BLOB/dates/...). Both approaches were tried directly against a live
 * MariaDB server: the null-buffer probe reliably reports a truncated
 * column's true length for STRING-family columns, but *not* for these
 * fixed-width numeric ones -- `*length` came back wrong (observed: every
 * FLOAT/DOUBLE column decoded as 0.0 regardless of its real value) even
 * though the fetch itself reported MYSQL_DATA_TRUNCATED as expected. A
 * fixed buffer sidesteps that rather than depending on it: 128 bytes is
 * far more than any of these types' string form ever needs (MySQL's own
 * DECIMAL/NEWDECIMAL max precision is 65 digits, so ~68 characters worst
 * case including sign and point). */
static bool mysql_fixed_width_field_helper(enum enum_field_types type) {
    switch(type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
            return true;
        default:
            return false;
    }
}

static DiamondVmStatus mysql_query_helper(DiamondVm *vm,DiamondMysqlHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,DiamondValue *result) {
    MYSQL_STMT *stmt=nullptr;
    const DiamondVmStatus status=mysql_exec_helper(vm,handle,sql_chars,sql_length,
        param_values,param_count,&stmt);
    if(status!=DIAMOND_VM_OK)return status;
    MYSQL_RES *meta=mysql_stmt_result_metadata(stmt);
    if(meta==nullptr) {
        /* Not a resultset-producing statement (e.g. an UPDATE run through
         * #query) -- an empty Array, the same "no rows" shape a SELECT
         * matching nothing produces. */
        DiamondArray *rows=allocate_array(vm,nullptr,0);
        if(rows==nullptr) {mysql_stmt_close(stmt);return DIAMOND_VM_OUT_OF_MEMORY;}
        *result=DIAMOND_OBJECT(rows);
        mysql_stmt_close(stmt);
        return DIAMOND_VM_OK;
    }
    const unsigned int column_count=mysql_num_fields(meta);
    MYSQL_FIELD *fields=mysql_fetch_fields(meta);
    MYSQL_BIND *out_binds=calloc(column_count,sizeof(MYSQL_BIND));
    unsigned long *lengths=calloc(column_count,sizeof(unsigned long));
    my_bool *nulls=calloc(column_count,sizeof(my_bool));
    char *fixed_buffers=calloc(column_count,DIAMOND_MYSQL_FIXED_FIELD_WIDTH);
    if(out_binds==nullptr||lengths==nullptr||nulls==nullptr||fixed_buffers==nullptr) {
        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
        mysql_free_result(meta);mysql_stmt_close(stmt);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    for(unsigned int col=0;col<column_count;col++) {
        out_binds[col].buffer_type=MYSQL_TYPE_STRING;
        out_binds[col].length=&lengths[col];
        out_binds[col].is_null=&nulls[col];
        if(mysql_fixed_width_field_helper(fields[col].type)) {
            out_binds[col].buffer=fixed_buffers+(size_t)col*DIAMOND_MYSQL_FIXED_FIELD_WIDTH;
            out_binds[col].buffer_length=DIAMOND_MYSQL_FIXED_FIELD_WIDTH;
        }
    }
    if(mysql_stmt_bind_result(stmt,out_binds)!=0) {
        snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
        mysql_free_result(meta);mysql_stmt_close(stmt);
        return DIAMOND_VM_MYSQL_ERROR;
    }
    if(mysql_stmt_store_result(stmt)!=0) {
        snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
        mysql_free_result(meta);mysql_stmt_close(stmt);
        return DIAMOND_VM_MYSQL_ERROR;
    }
    DiamondArray *rows=allocate_array(vm,nullptr,0);
    if(rows==nullptr) {
        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
        mysql_free_result(meta);mysql_stmt_close(stmt);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *result=DIAMOND_OBJECT(rows);
    for(;;) {
        const int fetch_status=mysql_stmt_fetch(stmt);
        if(fetch_status==MYSQL_NO_DATA)break;
        if(fetch_status!=0&&fetch_status!=MYSQL_DATA_TRUNCATED) {
            snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
            free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
            mysql_free_result(meta);mysql_stmt_close(stmt);
            return DIAMOND_VM_MYSQL_ERROR;
        }
        DiamondHash *row_hash=allocate_hash(vm);
        if(row_hash==nullptr) {
            free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
            mysql_free_result(meta);mysql_stmt_close(stmt);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        if(!array_push(vm,rows,DIAMOND_OBJECT(row_hash))) {
            free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
            mysql_free_result(meta);mysql_stmt_close(stmt);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        for(unsigned int col=0;col<column_count;col++) {
            DiamondString *key=
                allocate_string(vm,fields[col].name,strlen(fields[col].name));
            if(key==nullptr) {
                free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                mysql_free_result(meta);mysql_stmt_close(stmt);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            if(!hash_set(vm,row_hash,DIAMOND_OBJECT(key),DIAMOND_NIL)) {
                free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                mysql_free_result(meta);mysql_stmt_close(stmt);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            DiamondValue column_value=DIAMOND_NIL;
            if(!nulls[col]) {
                const bool fixed_width=mysql_fixed_width_field_helper(fields[col].type);
                bool decoded;
                if(fixed_width) {
                    decoded=mysql_decode_value_helper(vm,&fields[col],
                        fixed_buffers+(size_t)col*DIAMOND_MYSQL_FIXED_FIELD_WIDTH,
                        lengths[col],&column_value);
                } else {
                    const unsigned long value_length=lengths[col];
                    char *buffer=malloc(value_length>0?value_length:1);
                    if(buffer==nullptr) {
                        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                        mysql_free_result(meta);mysql_stmt_close(stmt);
                        return DIAMOND_VM_OUT_OF_MEMORY;
                    }
                    MYSQL_BIND fetch_bind=(MYSQL_BIND){0};
                    fetch_bind.buffer_type=MYSQL_TYPE_STRING;
                    fetch_bind.buffer=buffer;
                    fetch_bind.buffer_length=value_length;
                    if(mysql_stmt_fetch_column(stmt,&fetch_bind,col,0)!=0) {
                        snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
                        free(buffer);
                        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                        mysql_free_result(meta);mysql_stmt_close(stmt);
                        return DIAMOND_VM_MYSQL_ERROR;
                    }
                    decoded=mysql_decode_value_helper(vm,&fields[col],buffer,
                        value_length,&column_value);
                    free(buffer);
                }
                if(!decoded) {
                    free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                    mysql_free_result(meta);mysql_stmt_close(stmt);
                    return DIAMOND_VM_OUT_OF_MEMORY;
                }
            }
            if(!hash_set(vm,row_hash,DIAMOND_OBJECT(key),column_value)) {
                free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                mysql_free_result(meta);mysql_stmt_close(stmt);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
        }
    }
    free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
    mysql_free_result(meta);
    mysql_stmt_close(stmt);
    return DIAMOND_VM_OK;
}

/* #execute/#query/#last_insert_row_id/#close -- factored out of the
 * INVOKE case body for the same stack-frame reason sqlite3_dispatch_
 * helper's own comment explains. #last_insert_row_id is a direct
 * mysql_insert_id(conn) call, simpler than PostgreSQL's own `SELECT
 * lastval()` round-trip -- MySQL's client library tracks the connection's
 * last AUTO_INCREMENT value itself, no separate query needed, and (unlike
 * lastval()) it has no "not yet defined this session" failure mode: an
 * unused connection just reads back 0. */
DiamondVmStatus mysql_dispatch_helper(DiamondVm *vm,DiamondMysqlHandle *target_db,
        const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base,
        uint8_t argc,uint16_t dest) {
    const bool execute_method=method_name->length==7&&
        memcmp(method_name->chars,"execute",7)==0;
    const bool query_method=method_name->length==5&&
        memcmp(method_name->chars,"query",5)==0;
    const bool last_insert_row_id_method=method_name->length==18&&
        memcmp(method_name->chars,"last_insert_row_id",18)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    if(!execute_method&&!query_method&&!last_insert_row_id_method&&!close_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"MySQL");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(target_db->conn!=nullptr) {
            mysql_close(target_db->conn);
            target_db->conn=nullptr;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(target_db->conn==nullptr) {
        snprintf(vm->error,sizeof vm->error,"MySQL connection is closed");
        return DIAMOND_VM_MYSQL_ERROR;
    }
    if(last_insert_row_id_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_INT((int64_t)mysql_insert_id(target_db->conn));
        return DIAMOND_VM_OK;
    }
    /* execute/query share the same argument shape: (sql) or (sql, params). */
    if(argc!=1&&argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"MySQL#%.*s's sql argument must be a String",
            (int)method_name->length,method_name->chars);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *sql=(const DiamondString *)registers[base].as.object;
    const DiamondValue *param_values=nullptr;
    size_t param_count=0;
    if(argc==2) {
        if(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
            snprintf(vm->error,sizeof vm->error,
                "MySQL#%.*s's params argument must be an Array",
                (int)method_name->length,method_name->chars);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondArray *params=(const DiamondArray *)registers[(size_t)base+1].as.object;
        param_values=params->values;
        param_count=params->count;
    }
    if(execute_method) {
        DiamondValue execute_result=DIAMOND_NIL;
        const DiamondVmStatus execute_status=mysql_execute_helper(vm,
            target_db,sql->chars,sql->length,param_values,param_count,&execute_result);
        if(execute_status!=DIAMOND_VM_OK)return execute_status;
        registers[dest]=execute_result;return DIAMOND_VM_OK;
    }
    /* query_method: mysql_query_helper writes directly into registers[dest]
     * (a real GC root), not a local -- see its own comment. */
    return mysql_query_helper(vm,target_db,sql->chars,sql->length,param_values,
        param_count,&registers[dest]);
}
