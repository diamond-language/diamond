#include "dependencies.h"

#include <stdlib.h>
#include <string.h>

typedef struct DependencyEdge {
    char *uri;
    char *path;
} DependencyEdge;

struct DependencyTable {
    DependencyEdge *edges;
    size_t count;
    size_t capacity;
};

DependencyTable *dependency_table_create(void) {
    return calloc(1,sizeof(DependencyTable));
}

void dependency_table_free(DependencyTable *table) {
    if(table==nullptr)return;
    for(size_t index=0;index<table->count;index++) {
        free(table->edges[index].uri);
        free(table->edges[index].path);
    }
    free(table->edges);
    free(table);
}

static char *copy_string(const char *chars) {
    const size_t length=strlen(chars);
    char *copy=malloc(length+1);
    if(copy==nullptr)return nullptr;
    memcpy(copy,chars,length+1);
    return copy;
}

void dependency_table_remove_document(DependencyTable *table,const char *uri) {
    size_t index=0;
    while(index<table->count) {
        if(strcmp(table->edges[index].uri,uri)==0) {
            free(table->edges[index].uri);
            free(table->edges[index].path);
            table->edges[index]=table->edges[table->count-1];
            table->count--;
        } else index++;
    }
}

static bool push_edge(DependencyTable *table,const char *uri,const char *path) {
    if(table->count==table->capacity) {
        const size_t capacity=table->capacity==0?8:table->capacity*2;
        DependencyEdge *grown=realloc(table->edges,capacity*sizeof(DependencyEdge));
        if(grown==nullptr)return false;
        table->edges=grown;
        table->capacity=capacity;
    }
    char *uri_copy=copy_string(uri);
    char *path_copy=copy_string(path);
    if(uri_copy==nullptr||path_copy==nullptr) {
        free(uri_copy);free(path_copy);
        return false;
    }
    table->edges[table->count++]=(DependencyEdge){.uri=uri_copy,.path=path_copy};
    return true;
}

bool dependency_table_update(DependencyTable *table,const char *uri,
        const JsonValue *paths) {
    DependencyTable replacement={0};
    if(paths==nullptr||paths->kind!=JSON_ARRAY) {
        dependency_table_remove_document(table,uri);
        return true;
    }
    for(size_t index=0;index<paths->as.array.count;index++) {
        const JsonValue *entry=paths->as.array.items[index];
        if(entry->kind!=JSON_STRING)continue;
        bool duplicate=false;
        for(size_t existing=0;existing<replacement.count;existing++) {
            if(strcmp(replacement.edges[existing].path,entry->as.string.chars)==0) {
                duplicate=true;break;
            }
        }
        if(duplicate)continue;
        if(!push_edge(&replacement,uri,entry->as.string.chars)) {
            for(size_t free_index=0;free_index<replacement.count;free_index++) {
                free(replacement.edges[free_index].uri);
                free(replacement.edges[free_index].path);
            }
            free(replacement.edges);
            return false;
        }
    }
    dependency_table_remove_document(table,uri);
    for(size_t index=0;index<replacement.count;index++)
        push_edge(table,replacement.edges[index].uri,replacement.edges[index].path);
    for(size_t free_index=0;free_index<replacement.count;free_index++) {
        free(replacement.edges[free_index].uri);
        free(replacement.edges[free_index].path);
    }
    free(replacement.edges);
    return true;
}

char **dependency_table_dependents(const DependencyTable *table,const char *path,
        size_t *out_count) {
    *out_count=0;
    size_t capacity=0;
    char **results=nullptr;
    for(size_t index=0;index<table->count;index++) {
        if(strcmp(table->edges[index].path,path)!=0)continue;
        if(*out_count==capacity) {
            capacity=capacity==0?4:capacity*2;
            char **grown=realloc(results,capacity*sizeof(char *));
            if(grown==nullptr) {
                for(size_t free_index=0;free_index<*out_count;free_index++)
                    free(results[free_index]);
                free(results);
                *out_count=0;
                return nullptr;
            }
            results=grown;
        }
        char *uri_copy=copy_string(table->edges[index].uri);
        if(uri_copy==nullptr) {
            for(size_t free_index=0;free_index<*out_count;free_index++)
                free(results[free_index]);
            free(results);
            *out_count=0;
            return nullptr;
        }
        results[(*out_count)++]=uri_copy;
    }
    return results;
}
