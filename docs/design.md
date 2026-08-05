# Diamond design

Diamond is a personal research language. These notes describe the current
implementation, followed by decisions that are intentionally still open.

## Goals

- Ruby-like, expression-oriented syntax and object semantics.
- Gradual typing without a separate typed object model.
- A custom VM understandable from lexer through garbage collector.
- Fast experimentation over compatibility, portability, or ecosystem breadth.

## Implementation

Diamond is implemented in C23 and currently targets Linux with GCC 16. It uses
a register bytecode VM with 256 registers per call frame. Bytecode instructions
have one-byte opcodes and explicitly decoded operands; jumps contain absolute
16-bit bytecode offsets.

Runtime values use an explicit tagged union. Immediate values are `nil`,
booleans, and signed 64-bit integers. Strings, arrays, hashes, and instances are
managed heap objects with a common header. NaN boxing is deferred until
measurement shows that representation density is worth the complexity.

Integer arithmetic uses C23 checked arithmetic and reports overflow rather than
invoking C undefined behavior.

## Compilation

The lexer records byte offsets, lines, and columns. A Pratt parser compiles
expressions directly to register bytecode; there is no retained AST. Locals and
temporary values currently receive monotonically increasing registers within a
function, capped at 256.

Each opcode carries the line and column of the source token that produced it.
The disassembler displays these coordinates, and the VM uses them while
unwinding failed calls to build Diamond-level stack traces without exposing C
implementation frames.

`raise value` emits a dedicated exception opcode. The VM roots the raised
Diamond value and propagates a distinct exception status through functions,
methods, and closures, appending the same source-mapped frames used by runtime
errors. Bytecode-level rescue handlers are the next exception milestone.

The compiler tracks exact types for locally obvious temporary values. It removes
provably redundant type guards, rejects provable mismatches, and leaves runtime
guards at dynamic boundaries. Mutable and uncertain flows are treated
conservatively.

## Object model

Classes are immutable module metadata rather than heap objects. Instances point
to their class and contain a fixed field array. Instance-variable names are
assigned stable class-owned offsets during compilation; subclasses copy their
parent's field-slot prefix.

Methods are bytecode functions with `self` in register zero. Dynamic method
lookup walks the receiver's class and superclass chain. `super(arguments)` is
anchored to the class that lexically defined the calling method.

Each class owns a pointer-stable chain of shapes representing materialized field
prefixes. Fresh instances begin at shape zero; writing a field advances to the
shape that includes its slot, while an unmaterialized read produces `nil`.
Method dispatch is independent of field state, so each dynamic invoke bytecode
site uses a VM-owned four-entry polymorphic cache guarded by class pointer. A hit
bypasses lookup; a miss performs normal lookup and fills or replaces an entry.

Nested functions compile to heap-allocated closure objects. A closure identifies
its bytecode function and traces captured values through the garbage collector;
dynamic closure calls use a separate opcode from statically resolved top-level
calls. Locals are boxed only when captured; sibling closures share the resulting
GC-traced mutable cell, while ordinary locals remain direct registers. Nested
closures forward cells through intermediate environments, preserving identity
and mutation across arbitrary lexical depth.

See [object-model.md](object-model.md) for layouts and current limitations.

## Memory management

Each VM owns a linked list of heap objects. Collection is stop-the-world
mark/sweep. Active register frames are explicit roots; instances trace fields,
arrays trace elements, and hashes trace keys and values. Strings are leaf
objects. `DIAMOND_STRESS_GC=1` collects before every eligible allocation to test
rooting paths.

The collector is non-generational and non-moving, so mutations do not require a
write barrier. Object finalizers and weak references do not exist.

## Gradual types

Annotations describe acceptable runtime values and do not alter value layout.
Supported types are `Int`, `String`, `Bool`, `Nil`, `Array`, `Hash`, and declared
classes. Nominal class checks accept subclasses. `Type | Nil` provides compact
nilable unions.

Annotations are optional. Parameters are checked on function entry and return
values on every implicit or explicit exit unless the compiler proves the guard
redundant.

## Deliberate constraints

- No Ruby compatibility guarantee.
- No closures, modules, exceptions, generics, or general unions yet.
- No native-code generator or JIT.
- No stable bytecode, embedding API, or package format.
- No parallel execution.
- No portability target beyond the current development machine.
