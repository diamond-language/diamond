#include "document.h"

#include "diagnostics.h"

#include <stdlib.h>
#include <string.h>

typedef struct Document {
    char *uri;
    char *text;
    size_t length;
} Document;

struct DocumentTable {
    Document *documents;
    size_t count;
    size_t capacity;
};

DocumentTable *document_table_create(void) {
    return calloc(1,sizeof(DocumentTable));
}

void document_table_free(DocumentTable *table) {
    if(table==nullptr)return;
    for(size_t index=0;index<table->count;index++) {
        free(table->documents[index].uri);
        free(table->documents[index].text);
    }
    free(table->documents);
    free(table);
}

static Document *find_document(DocumentTable *table,const char *uri) {
    for(size_t index=0;index<table->count;index++)
        if(strcmp(table->documents[index].uri,uri)==0)
            return &table->documents[index];
    return nullptr;
}

static char *copy_bytes(const char *bytes,size_t length) {
    char *copy=malloc(length+1);
    if(copy==nullptr)return nullptr;
    memcpy(copy,bytes,length);
    copy[length]='\0';
    return copy;
}

bool document_open(DocumentTable *table,const char *uri,const char *text,size_t length) {
    char *text_copy=copy_bytes(text,length);
    if(text_copy==nullptr)return false;
    Document *existing=find_document(table,uri);
    if(existing!=nullptr) {
        free(existing->text);
        existing->text=text_copy;
        existing->length=length;
        return true;
    }
    if(table->count==table->capacity) {
        size_t capacity=table->capacity==0?8:table->capacity*2;
        Document *grown=realloc(table->documents,capacity*sizeof(Document));
        if(grown==nullptr) {
            free(text_copy);
            return false;
        }
        table->documents=grown;
        table->capacity=capacity;
    }
    char *uri_copy=copy_bytes(uri,strlen(uri));
    if(uri_copy==nullptr) {
        free(text_copy);
        return false;
    }
    table->documents[table->count++]=(Document){.uri=uri_copy,.text=text_copy,.length=length};
    return true;
}

bool document_update(DocumentTable *table,const char *uri,const char *text,size_t length) {
    Document *existing=find_document(table,uri);
    if(existing==nullptr)return false;
    char *text_copy=copy_bytes(text,length);
    if(text_copy==nullptr)return false;
    free(existing->text);
    existing->text=text_copy;
    existing->length=length;
    return true;
}

void document_close(DocumentTable *table,const char *uri) {
    for(size_t index=0;index<table->count;index++) {
        if(strcmp(table->documents[index].uri,uri)==0) {
            free(table->documents[index].uri);
            free(table->documents[index].text);
            table->documents[index]=table->documents[table->count-1];
            table->count--;
            return;
        }
    }
}

const char *document_get_text(const DocumentTable *table,const char *uri,size_t *length) {
    for(size_t index=0;index<table->count;index++) {
        if(strcmp(table->documents[index].uri,uri)==0) {
            if(length!=nullptr)*length=table->documents[index].length;
            return table->documents[index].text;
        }
    }
    return nullptr;
}

char *document_resolve_source(const char *path,void *user_data) {
    const DocumentTable *table=(const DocumentTable *)user_data;
    char *uri=diagnostics_path_to_uri(path);
    if(uri==nullptr)return nullptr;
    size_t length=0;
    const char *text=document_get_text(table,uri,&length);
    free(uri);
    if(text==nullptr)return nullptr;
    return copy_bytes(text,length);
}
