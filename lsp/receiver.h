#ifndef DIAMOND_LSP_RECEIVER_H
#define DIAMOND_LSP_RECEIVER_H

#include "compiler.h"

#include <stdbool.h>
#include <stddef.h>

/* Resolves the class(es) named by a `receiver.` expression's receiver,
 * for the forms the LSP understands (see docs/lsp.md): a literal class
 * name (`Author.find`), `self` inside an instance method or a
 * class-owned `def self.x` (wall 2, DIAMOND_VALUE_CLASS), a local
 * variable whose known type at its declaration was `ClassName.new(...)`
 * (DiamondScopeLocal.known_type, src/vm.h), or a parameter (or a local
 * initialized from one) given an explicit union annotation or representable
 * `if`/`unless`/ternary join (`x: Dog | Cat`, or
 * `x = flag ? Dog.new() : Cat.new()`, DiamondScopeLocal.known_type_set), an instance variable
 * whose assignments all agree on one class, or a call chain whose top-level
 * function/method links have explicit class return annotations. `Class.new()`
 * is intrinsically an instance of Class; all other links use the declared
 * DiamondFunction.return_type_set. Every class-kind union member is returned
 * as a candidate. Unknown/conflicting ivars and unannotated call returns
 * deliberately return 0 so callers fall back to their existing behavior.
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
 * On a match, writes up to `max_candidates` class indices into
 * `class_indices` (an index into chunk->classes each) and returns how
 * many -- 0 means no supported receiver form was found. Every candidate
 * shares one `is_singleton`: whether the receiver's method should be
 * looked up in each class's singleton_methods[] (self inside a
 * class-owned `def self.x`, or a literal class name -- always exactly
 * one candidate) or its ordinary methods[] (self inside an instance
 * method, or a local holding an instance or union of instances). A
 * caller only interested in the single-candidate case can just use
 * element 0 when the return value is 1. `max_candidates` should be at
 * least DIAMOND_MAX_UNION_TYPES (src/vm.h) to never truncate a real
 * union's own member count. */
size_t receiver_resolve_classes(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,size_t stop_offset,
        size_t *class_indices,size_t max_candidates,bool *is_singleton);

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
