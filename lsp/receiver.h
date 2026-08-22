#ifndef DIAMOND_LSP_RECEIVER_H
#define DIAMOND_LSP_RECEIVER_H

#include "compiler.h"

#include <stdbool.h>
#include <stddef.h>

/* Resolves the class named by a `receiver.` expression's receiver, for
 * the three deliberately scoped forms the LSP understands (see
 * docs/lsp.md): a literal class name (`Author.find`), `self` inside an
 * instance method or a class-owned `def self.x` (wall 2,
 * DIAMOND_VALUE_CLASS), or a local variable whose known type at its
 * declaration was `ClassName.new(...)` (DiamondScopeLocal.known_type,
 * src/vm.h). Everything else -- an ivar receiver, a chained call
 * (`foo().bar`), a union/ambiguous type -- deliberately returns false so
 * callers fall back to their existing non-receiver behavior.
 *
 * `source` is the raw, un-prelude-bundled open-document text (matching
 * what identifier_token_at already tokenizes in hover.c/definition.c).
 * `stop_offset` is a byte offset into it strictly before which every
 * token is considered part of the receiver expression: for hover/
 * definition, pass the method-name identifier's own span.start (the
 * cursor already sits on a complete identifier there); for completion
 * triggered right after `receiver.` (with the method name partially
 * typed or not yet typed at all), pass the raw cursor offset instead.
 *
 * On success, *class_index is an index into chunk->classes and
 * *is_singleton says whether the receiver's method should be looked up
 * in that class's singleton_methods[] (self inside a class-owned
 * `def self.x`, or a literal class name) or its ordinary methods[]
 * (self inside an instance method, or a local holding an instance). */
bool receiver_resolve_class(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,size_t stop_offset,
        size_t *class_index,bool *is_singleton);

/* Walks `class_index` and its superclass chain (chunk->classes[i].
 * superclass, UINT8_MAX-terminated) for a method named `name` --
 * methods[] if is_singleton is false, singleton_methods[] if true. The
 * same algorithm lookup_method/lookup_singleton_method (src/vm.c) use
 * at runtime against a live DiamondClass*, mirrored here over the
 * compile-time chunk->classes metadata the LSP actually has. Returns
 * nullptr if no method by that name exists anywhere in the chain. */
const DiamondMethod *receiver_lookup_method(const DiamondChunk *chunk,
        size_t class_index,bool is_singleton,const char *name,size_t name_length);

#endif
