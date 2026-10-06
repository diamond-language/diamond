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

/* Zero-filled rows x cols DiamondTensor -- struct + payload in one
 * malloc (DiamondTensor's own flexible array member), same allocation
 * shape as allocate_string/allocate_symbol. Overflow-checked: rows*cols
 * (element count) and its *sizeof(double) byte size both go through
 * size_t, and a pathological rows/cols pair (both runtime Ints, never
 * validated against any upper bound before reaching here) could
 * overflow either multiplication on a real -- if rare -- input rather
 * than just producing a huge-but-correct allocation request. */
DiamondTensor *allocate_tensor(DiamondVm *vm,size_t rows,size_t cols) {
    if (!maybe_collect(vm)) return nullptr;
    if(rows!=0&&cols>SIZE_MAX/rows)return nullptr;
    const size_t element_count=rows*cols;
    if(element_count!=0&&sizeof(double)>SIZE_MAX/element_count)return nullptr;
    const size_t payload_size=element_count*sizeof(double);
    if(payload_size>SIZE_MAX-sizeof(DiamondTensor))return nullptr;
    const size_t size=sizeof(DiamondTensor)+payload_size;
    DiamondTensor *tensor=malloc(size);
    if(tensor==nullptr)return nullptr;
    tensor->object=(DiamondObject){.next=vm->young_objects,.kind=DIAMOND_OBJECT_TENSOR};
    tensor->rows=rows;tensor->cols=cols;
    memset(tensor->data,0,payload_size);
    vm->young_objects=&tensor->object;vm->bytes_allocated+=size;return tensor;
}

/* Computes rows [row_start,row_end) of a (m x k) * b (k x n) -> c (m x
 * n), c already zero-filled by the caller (allocate_tensor). Two
 * layering decisions on top of the plain ikj loop order (see this
 * function's own history/git blame for that first pass, which is
 * still exactly the innermost triple here):
 *
 * 1. k is blocked (block_k rows of `b` at a time) with the *full* row
 *    range looped inside each block, not the other way around --
 *    for a fixed k-block, every row in [row_start,row_end) reuses
 *    that same slice of `b` (block_k*n doubles) before moving to the
 *    next block, instead of re-streaming the *entire* `b` matrix from
 *    memory once per row of `a`. block_k is sized (by the caller) so
 *    that slice comfortably fits in L2 -- without this, a k this
 *    large (b bigger than cache) means every row of `a` re-pays `b`'s
 *    full memory latency, which is exactly the FFN-shaped slowdown
 *    the plain ikj version measured (16 GFLOPS on a cache-resident
 *    512x512, 6.7 GFLOPS once k/n grow past cache).
 * 2. This is the per-thread body: the caller (tensor_matmul_helper)
 *    splits [0,m) into disjoint [row_start,row_end) ranges across
 *    however many worker threads it decides to use and calls this
 *    once per thread -- always correct with no locking, since
 *    distinct row ranges write disjoint rows of `c` and never touch
 *    each other's rows of `a`/`c` (`b` is read-only and shared).
 *
 * Reordering the k-block loop outermost changes the *order* summed
 * terms are added in, not *which* terms -- standard, expected
 * floating-point reassociation for a blocked matmul (real BLAS
 * implementations do the same), not a correctness bug: the result
 * differs from the unblocked version only in the last ULP or so of
 * rounding, same as reassociating any other FP sum. */
static void tensor_matmul_row_range(const double *restrict a,const double *restrict b,
        double *restrict c,size_t row_start,size_t row_end,size_t k,size_t n,size_t block_k) {
    for(size_t p0=0;p0<k;p0+=block_k) {
        const size_t p_end=p0+block_k<k?p0+block_k:k;
        for(size_t i=row_start;i<row_end;i++) {
            const double *restrict a_row=a+i*k;
            double *restrict c_row=c+i*n;
            for(size_t p=p0;p<p_end;p++) {
                const double a_ip=a_row[p];
                const double *restrict b_row=b+p*n;
                for(size_t j=0;j<n;j++) {
                    c_row[j]+=a_ip*b_row[j];
                }
            }
        }
    }
}

typedef struct TensorMatmulThreadArgs {
    const double *a,*b; double *c;
    size_t row_start,row_end,k,n,block_k;
} TensorMatmulThreadArgs;

static void *tensor_matmul_thread_entry(void *raw_args) {
    const TensorMatmulThreadArgs *args=(const TensorMatmulThreadArgs *)raw_args;
    tensor_matmul_row_range(args->a,args->b,args->c,
        args->row_start,args->row_end,args->k,args->n,args->block_k);
    return nullptr;
}

/* Caps how many worker threads a single matmul call will ever spawn --
 * a real inference/training loop calls #matmul constantly, so this
 * deliberately doesn't just grab every core sysconf reports every
 * single call (that's fine for one isolated benchmark, not for a
 * process also trying to do other work, or for several matmuls
 * in flight from different Fibers/Threads at once). 8 is a reasonable
 * fixed ceiling for a prototype; a real deployment-tunable value is a
 * follow-up, not a hard architectural limit. */
#define DIAMOND_TENSOR_MATMUL_MAX_THREADS 8

/* a (m x k) * b (k x n) -> a fresh (m x n) DiamondTensor. `*out` MUST be
 * a pointer into the caller's live registers array (&registers[dest]),
 * same GC-rooting requirement as sqlite3_query_helper's own `result` --
 * written immediately after allocate_tensor succeeds, before any
 * compute happens, even though nothing below allocates (no future
 * caller of this static function should assume that stays true
 * forever and drop the immediate-write discipline).
 *
 * block_k targets a fixed byte budget per k-block (see
 * tensor_matmul_row_range's own comment for why blocking k helps) --
 * TARGET_BLOCK_BYTES is a conservative guess at "comfortably inside
 * L2 on a typical desktop/server core" (256KB-1MB is common; 128KB
 * leaves headroom for the a_row/c_row lines simultaneously in flight),
 * not a measured-on-this-machine value -- tuning it against a real L2
 * size is a follow-up if the numbers call for it.
 *
 * Threading: splits [0,m) into up to DIAMOND_TENSOR_MATMUL_MAX_THREADS
 * (bounded further by hardware concurrency and by m itself -- no
 * point spawning more threads than output rows) contiguous, disjoint
 * row ranges, one call to tensor_matmul_row_range per thread, joined
 * before returning. Skipped entirely (falls straight through to a
 * single synchronous call) below DIAMOND_TENSOR_MATMUL_THREAD_FLOOR
 * total FLOPs -- thread spawn/join overhead would otherwise dominate
 * a transformer's many small per-head/per-token matmuls, not just the
 * few big FFN ones. */
#define DIAMOND_TENSOR_MATMUL_THREAD_FLOOR (1024ULL*1024ULL)
static DiamondVmStatus tensor_matmul_helper(DiamondVm *vm,const DiamondTensor *a,
        const DiamondTensor *b,DiamondValue *out) {
    if(a->cols!=b->rows) {
        snprintf(vm->error,sizeof vm->error,
            "Tensor#matmul shape mismatch: %zux%zu * %zux%zu",
            a->rows,a->cols,b->rows,b->cols);
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondTensor *result=allocate_tensor(vm,a->rows,b->cols);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(result);
    const size_t m=a->rows,k=a->cols,n=b->cols;
    const size_t target_block_bytes=128*1024;
    size_t block_k=target_block_bytes/(n*sizeof(double));
    if(block_k<1)block_k=1;
    if(block_k>k)block_k=k;
    size_t thread_count=1;
    const unsigned long long total_flops=2ULL*(unsigned long long)m*
        (unsigned long long)k*(unsigned long long)n;
    if(total_flops>=DIAMOND_TENSOR_MATMUL_THREAD_FLOOR&&m>1) {
        const long online=sysconf(_SC_NPROCESSORS_ONLN);
        size_t available=online>0?(size_t)online:1;
        if(available>DIAMOND_TENSOR_MATMUL_MAX_THREADS)
            available=DIAMOND_TENSOR_MATMUL_MAX_THREADS;
        thread_count=available<m?available:m;
    }
    if(thread_count<=1) {
        tensor_matmul_row_range(a->data,b->data,result->data,0,m,k,n,block_k);
        return DIAMOND_VM_OK;
    }
    pthread_t threads[DIAMOND_TENSOR_MATMUL_MAX_THREADS];
    TensorMatmulThreadArgs thread_args[DIAMOND_TENSOR_MATMUL_MAX_THREADS];
    const size_t base_rows_per_thread=m/thread_count;
    const size_t extra_rows=m%thread_count;
    size_t row_cursor=0;
    size_t spawned=0;
    for(size_t index=0;index<thread_count;index++) {
        const size_t this_thread_rows=base_rows_per_thread+(index<extra_rows?1:0);
        const size_t row_start=row_cursor;
        const size_t row_end=row_cursor+this_thread_rows;
        row_cursor=row_end;
        thread_args[index]=(TensorMatmulThreadArgs){.a=a->data,.b=b->data,.c=result->data,
            .row_start=row_start,.row_end=row_end,.k=k,.n=n,.block_k=block_k};
        /* Last range runs on this thread instead of spawning one more --
         * avoids paying a spawn+join for a thread_count'th of the work
         * when this thread is sitting idle waiting for the others
         * anyway. */
        if(index+1==thread_count) {
            tensor_matmul_row_range(a->data,b->data,result->data,row_start,row_end,k,n,block_k);
            continue;
        }
        if(pthread_create(&threads[index],nullptr,
                tensor_matmul_thread_entry,&thread_args[index])!=0) {
            /* Spawn failed partway through -- finish this and every
             * remaining range synchronously on this thread rather than
             * leaving rows [row_start,m) uncomputed, then join whatever
             * did successfully spawn before returning. */
            tensor_matmul_row_range(a->data,b->data,result->data,row_start,m,k,n,block_k);
            for(size_t joined=0;joined<index;joined++)pthread_join(threads[joined],nullptr);
            return DIAMOND_VM_OK;
        }
        spawned++;
    }
    for(size_t index=0;index<spawned;index++)pthread_join(threads[index],nullptr);
    return DIAMOND_VM_OK;
}

/* Tensor#to_a -- a nested Array-of-Arrays-of-Float snapshot, for
 * inspection/interop/tests at small scale (every element is a
 * separate boxed Float, so this is not meant for the hot path). Same
 * &registers[dest]-must-be-a-real-GC-root requirement, and same
 * "root the outer Array immediately, push each row onto it before
 * populating that row" discipline, as sqlite3_collect_rows_helper's
 * own comment explains in full -- DIAMOND_FLOAT itself never
 * allocates (Float lives directly in the tagged union, not as a heap
 * object), so a row Array stays the only allocation happening while
 * its own elements are appended. */
static DiamondVmStatus tensor_to_a_helper(DiamondVm *vm,const DiamondTensor *tensor,
        DiamondValue *out) {
    DiamondArray *outer=allocate_array(vm,nullptr,0);
    if(outer==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(outer);
    for(size_t i=0;i<tensor->rows;i++) {
        DiamondArray *row=allocate_array(vm,nullptr,0);
        if(row==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        if(!array_push(vm,outer,DIAMOND_OBJECT(row)))return DIAMOND_VM_OUT_OF_MEMORY;
        for(size_t j=0;j<tensor->cols;j++) {
            if(!array_push(vm,row,DIAMOND_FLOAT(tensor->data[i*tensor->cols+j])))
                return DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    return DIAMOND_VM_OK;
}

/* Tensor#transpose -- a fresh (cols x rows) copy, `*out` under the same
 * &registers[dest] GC-rooting requirement as tensor_matmul_helper's own
 * `out`. Not in-place (that's only free for a square Tensor; keeping
 * one #transpose semantics for every shape, always a fresh copy, is
 * simpler than a shape-dependent special case) -- needed for attention
 * (scores = Q.matmul(K.transpose())), not a hot O(n^3) path like matmul
 * itself, so the plain nested loop here (no blocking/threading) is
 * fine. */
static DiamondVmStatus tensor_transpose_helper(DiamondVm *vm,const DiamondTensor *tensor,
        DiamondValue *out) {
    DiamondTensor *result=allocate_tensor(vm,tensor->cols,tensor->rows);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(result);
    for(size_t i=0;i<tensor->rows;i++) {
        for(size_t j=0;j<tensor->cols;j++) {
            result->data[j*tensor->rows+i]=tensor->data[i*tensor->cols+j];
        }
    }
    return DIAMOND_VM_OK;
}

/* Tensor.from_array(nested_array) -- nested_array must be a non-empty
 * Array of non-empty Arrays, every row the same length, every element
 * Int or Float. Two full passes (validate, then fill) rather than
 * allocating the Tensor speculatively and unwinding on a later bad
 * element -- simpler, and this isn't a hot path (unlike matmul, it
 * runs once per weight/input load, not once per training/inference
 * step). */
DiamondVmStatus tensor_from_array_helper(DiamondVm *vm,const DiamondArray *outer,
        DiamondValue *out) {
    if(outer->count==0) {
        snprintf(vm->error,sizeof vm->error,"Tensor.from_array: outer Array must not be empty");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(outer->values[0].kind!=DIAMOND_VALUE_OBJECT||
       outer->values[0].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
        snprintf(vm->error,sizeof vm->error,"Tensor.from_array: every row must be an Array");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const size_t cols=((const DiamondArray *)outer->values[0].as.object)->count;
    if(cols==0) {
        snprintf(vm->error,sizeof vm->error,"Tensor.from_array: rows must not be empty");
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t i=0;i<outer->count;i++) {
        if(outer->values[i].kind!=DIAMOND_VALUE_OBJECT||
           outer->values[i].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
            snprintf(vm->error,sizeof vm->error,"Tensor.from_array: every row must be an Array");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondArray *row=(const DiamondArray *)outer->values[i].as.object;
        if(row->count!=cols) {
            snprintf(vm->error,sizeof vm->error,
                "Tensor.from_array: every row must have the same length (row 0 has %zu, row %zu has %zu)",
                cols,i,row->count);
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t j=0;j<cols;j++) {
            const DiamondValue element=row->values[j];
            if(element.kind!=DIAMOND_VALUE_INT&&element.kind!=DIAMOND_VALUE_FLOAT) {
                snprintf(vm->error,sizeof vm->error,
                    "Tensor.from_array: every element must be an Int or a Float");
                return DIAMOND_VM_TYPE_ERROR;
            }
        }
    }
    DiamondTensor *tensor=allocate_tensor(vm,outer->count,cols);
    if(tensor==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(tensor);
    for(size_t i=0;i<outer->count;i++) {
        const DiamondArray *row=(const DiamondArray *)outer->values[i].as.object;
        for(size_t j=0;j<cols;j++) {
            const DiamondValue element=row->values[j];
            tensor->data[i*cols+j]=
                element.kind==DIAMOND_VALUE_INT?(double)element.as.integer:element.as.real;
        }
    }
    return DIAMOND_VM_OK;
}

/* Tensor.random(rows, cols, seed) -- fills a fresh Tensor with
 * deterministic pseudorandom values in [-1, 1) directly in C, entirely
 * bypassing the slow path Tensor.from_array requires (build a
 * Diamond-level nested Array element by element via interpreted
 * #push, then re-walk and re-validate the whole thing) -- weight init
 * at any real model size measurably dominated build time in
 * examples/transformer (a vocab_size x d_model embedding table alone
 * is easily millions of elements) even though it's the one-time setup
 * cost, not the hot #matmul path.
 *
 * A plain LCG (same recurrence as examples/transformer/lib/rng.di's
 * own SimpleRng, reimplemented here rather than shared -- this is C,
 * that's Diamond) -- not cryptographic, doesn't need to be: same seed
 * always produces the same Tensor, which is all reproducible weight
 * init actually requires. `state` is uint64_t specifically so the
 * multiply can't silently wrap/misbehave the way it would in a
 * narrower type before the modulus brings it back into 31-bit range. */
void tensor_random_helper(DiamondTensor *tensor,int64_t seed) {
    uint64_t state=(uint64_t)seed;
    const size_t total=tensor->rows*tensor->cols;
    for(size_t index=0;index<total;index++) {
        state=(state*1103515245ULL+12345ULL)%2147483648ULL;
        const double sample=(double)state/2147483648.0;
        tensor->data[index]=sample*2.0-1.0;
    }
}

/* Elementwise/shape mutators -- the training-loop counterpart to
 * #matmul's own native speedup. Found the hard way: at
 * examples/transformer's real training config (d_model=128, 4 layers,
 * seq_len=64), matmuls are mostly *too small* to even cross
 * DIAMOND_TENSOR_MATMUL_THREAD_FLOOR, so #matmul was never the
 * bottleneck there -- one training step measured 5.55s, and the real
 * cost was tensor_ops.di's own per-element Diamond loops (layernorm,
 * softmax, gelu, add_bias, column slicing), each doing thousands of
 * individual #get/#set native-dispatch round-trips (memcmp method-name
 * matching, bounds checks, register shuffling) per call instead of one
 * tight C loop. At that rate a single ~140MB TinyStories shard
 * (100k stories) would take on the order of 90 days to train one pass
 * over. These mirror tensor_ops.di's own Diamond functions exactly
 * (same names/semantics, in-place, returning the mutated Tensor) so
 * that file can become a thin wrapper delegating to these instead of
 * rewriting any of autograd.di's own backward-formula math. */
static void tensor_add_inplace_helper(DiamondTensor *a,const DiamondTensor *b) {
    const size_t total=a->rows*a->cols;
    for(size_t index=0;index<total;index++) a->data[index]+=b->data[index];
}

static void tensor_scale_inplace_helper(DiamondTensor *x,double scalar) {
    const size_t total=x->rows*x->cols;
    for(size_t index=0;index<total;index++) x->data[index]*=scalar;
}

/* bias is always a single row (1 x x->cols), broadcast-added to every
 * row of x. */
static void tensor_add_bias_inplace_helper(DiamondTensor *x,const DiamondTensor *bias) {
    for(size_t row=0;row<x->rows;row++) {
        double *row_data=x->data+row*x->cols;
        for(size_t col=0;col<x->cols;col++) row_data[col]+=bias->data[col];
    }
}

static void tensor_row_softmax_inplace_helper(DiamondTensor *x) {
    for(size_t row=0;row<x->rows;row++) {
        double *row_data=x->data+row*x->cols;
        double max_value=row_data[0];
        for(size_t col=1;col<x->cols;col++)
            if(row_data[col]>max_value)max_value=row_data[col];
        double sum=0.0;
        for(size_t col=0;col<x->cols;col++) {
            row_data[col]=exp(row_data[col]-max_value);
            sum+=row_data[col];
        }
        for(size_t col=0;col<x->cols;col++) row_data[col]/=sum;
    }
}

/* GELU, tanh approximation -- same formula as tensor_ops.di's own
 * tensor_gelu!/scalar_tanh, but using libm's real tanh() directly
 * (this is C, not a language with no tanh() at all, unlike Diamond --
 * see scalar_tanh's own comment for why *that* one has to build it out
 * of exp() by hand). */
static void tensor_gelu_inplace_helper(DiamondTensor *x) {
    const size_t total=x->rows*x->cols;
    for(size_t index=0;index<total;index++) {
        const double value=x->data[index];
        const double inner=0.7978845608028654*(value+0.044715*value*value*value);
        x->data[index]=0.5*value*(1.0+tanh(inner));
    }
}

/* Sum of every row, as a fresh (1 x x->cols) DiamondTensor -- the
 * backward-pass shape for a bias that was broadcast-added to every row
 * in the forward pass (tensor_add_bias_inplace_helper's own gradient).
 * `*out` under the same &registers[dest] GC-rooting requirement as
 * every other allocating Tensor helper (tensor_matmul_helper, etc). */
static DiamondVmStatus tensor_column_sums_helper(DiamondVm *vm,const DiamondTensor *x,
        DiamondValue *out) {
    DiamondTensor *result=allocate_tensor(vm,1,x->cols);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(result);
    for(size_t row=0;row<x->rows;row++) {
        const double *row_data=x->data+row*x->cols;
        for(size_t col=0;col<x->cols;col++) result->data[col]+=row_data[col];
    }
    return DIAMOND_VM_OK;
}

/* Columns [start, start+width) of x, as a fresh Tensor -- splits a QKV
 * projection's d_model columns into one attention head's own slice
 * (same semantics as tensor_ops.di's own tensor_columns). */
static DiamondVmStatus tensor_columns_helper(DiamondVm *vm,const DiamondTensor *x,
        size_t start,size_t width,DiamondValue *out) {
    DiamondTensor *result=allocate_tensor(vm,x->rows,width);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(result);
    for(size_t row=0;row<x->rows;row++) {
        const double *source_row=x->data+row*x->cols+start;
        double *dest_row=result->data+row*width;
        for(size_t col=0;col<width;col++) dest_row[col]=source_row[col];
    }
    return DIAMOND_VM_OK;
}

/* Adds src into dest's columns [start, start+src->cols), in place --
 * accumulated (not overwritten), since that range may receive
 * contributions from more than one op (concat_columns's own backward,
 * where each head's slice could in principle overlap -- it doesn't in
 * practice here, but this stays correct either way). Same semantics as
 * tensor_ops.di's own tensor_add_columns!. */
static void tensor_add_columns_inplace_helper(DiamondTensor *dest,size_t start,
        const DiamondTensor *src) {
    for(size_t row=0;row<src->rows;row++) {
        double *dest_row=dest->data+row*dest->cols+start;
        const double *src_row=src->data+row*src->cols;
        for(size_t col=0;col<src->cols;col++) dest_row[col]+=src_row[col];
    }
}

/* Backward passes for the ops whose *forward* mutators are above but
 * whose gradient math still lived in examples/transformer's own
 * Autograd.softmax/#gelu/#layernorm (autograd.di) as plain Diamond
 * per-element loops -- moving only the forward mutators native
 * turned out not to be enough: a training step's own #matmul cost is
 * usually small at this model's scale (see DIAMOND_TENSOR_MATMUL_
 * MAX_THREADS's own comment on why), so forward and backward cost
 * roughly the same, and backward! calls every op's backward closure
 * exactly once per step just like forward calls its op once -- an
 * unmoved backward loop is just as much of the total step time as its
 * matching forward loop was.
 *
 * `self` in each of these is *not* always "the same value forward took
 * self as" -- softmax_backward's self is the softmax's own *output*
 * (its backward formula only needs that, not the pre-softmax input);
 * gelu_backward's self is GELU's original *input* (its formula needs
 * that, not the output) -- see each one's own comment for why.
 * layernorm's forward/backward pair is the most involved: forward
 * returns not just its own output but also `normalized` and per-row
 * `inv_std`, exactly the two things the standard LayerNorm backward
 * formula needs and that recomputing from scratch during backward
 * would otherwise cost a second full pass over x for. */
static DiamondVmStatus tensor_softmax_backward_helper(DiamondVm *vm,
        const DiamondTensor *softmax_output,const DiamondTensor *grad_output,
        DiamondValue *out) {
    DiamondTensor *result=allocate_tensor(vm,softmax_output->rows,softmax_output->cols);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(result);
    for(size_t row=0;row<softmax_output->rows;row++) {
        const double *y_row=softmax_output->data+row*softmax_output->cols;
        const double *dy_row=grad_output->data+row*grad_output->cols;
        double *dx_row=result->data+row*result->cols;
        double dot=0.0;
        for(size_t col=0;col<softmax_output->cols;col++) dot+=dy_row[col]*y_row[col];
        for(size_t col=0;col<softmax_output->cols;col++)
            dx_row[col]=y_row[col]*(dy_row[col]-dot);
    }
    return DIAMOND_VM_OK;
}

static DiamondVmStatus tensor_gelu_backward_helper(DiamondVm *vm,
        const DiamondTensor *x,const DiamondTensor *grad_output,DiamondValue *out) {
    DiamondTensor *result=allocate_tensor(vm,x->rows,x->cols);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(result);
    const size_t total=x->rows*x->cols;
    for(size_t index=0;index<total;index++) {
        const double value=x->data[index];
        const double inner=0.7978845608028654*(value+0.044715*value*value*value);
        const double t=tanh(inner);
        const double inner_derivative=0.7978845608028654*(1.0+3.0*0.044715*value*value);
        const double dy_dx=0.5*(1.0+t)+0.5*value*(1.0-t*t)*inner_derivative;
        result->data[index]=grad_output->data[index]*dy_dx;
    }
    return DIAMOND_VM_OK;
}

/* Returns [output, normalized, inv_std] (inv_std as a rows x 1
 * Tensor, one value per row) as a Diamond Array. `*out` rooting: same
 * &registers[dest]-must-be-a-live-GC-root requirement as every other
 * allocating Tensor helper, and the same "root the outer Array
 * immediately, push each element onto it before allocating the next"
 * discipline tensor_to_a_helper's own comment explains in full. */
static DiamondVmStatus tensor_layernorm_forward_helper(DiamondVm *vm,const DiamondTensor *x,
        const DiamondTensor *gamma,const DiamondTensor *beta,double eps,DiamondValue *out) {
    const size_t rows=x->rows,cols=x->cols;
    DiamondArray *results=allocate_array(vm,nullptr,0);
    if(results==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(results);

    DiamondTensor *output=allocate_tensor(vm,rows,cols);
    if(output==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!array_push(vm,results,DIAMOND_OBJECT(output)))return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondTensor *normalized=allocate_tensor(vm,rows,cols);
    if(normalized==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!array_push(vm,results,DIAMOND_OBJECT(normalized)))return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondTensor *inv_std=allocate_tensor(vm,rows,1);
    if(inv_std==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!array_push(vm,results,DIAMOND_OBJECT(inv_std)))return DIAMOND_VM_OUT_OF_MEMORY;

    for(size_t row=0;row<rows;row++) {
        const double *x_row=x->data+row*cols;
        double sum=0.0;
        for(size_t col=0;col<cols;col++) sum+=x_row[col];
        const double mean=sum/(double)cols;
        double variance_sum=0.0;
        for(size_t col=0;col<cols;col++) {
            const double diff=x_row[col]-mean;
            variance_sum+=diff*diff;
        }
        const double variance=variance_sum/(double)cols;
        const double inv_std_value=1.0/sqrt(variance+eps);
        inv_std->data[row]=inv_std_value;
        double *norm_row=normalized->data+row*cols;
        double *out_row=output->data+row*cols;
        for(size_t col=0;col<cols;col++) {
            const double n=(x_row[col]-mean)*inv_std_value;
            norm_row[col]=n;
            out_row[col]=n*gamma->data[col]+beta->data[col];
        }
    }
    return DIAMOND_VM_OK;
}

/* Returns [grad_x, grad_gamma, grad_beta] as a Diamond Array -- the
 * standard LayerNorm backward formula (see examples/transformer/lib/
 * autograd.di's own comment for the Diamond-level derivation this
 * mirrors exactly; this is the same math, just native). */
static DiamondVmStatus tensor_layernorm_backward_helper(DiamondVm *vm,
        const DiamondTensor *gamma,const DiamondTensor *grad_output,
        const DiamondTensor *normalized,const DiamondTensor *inv_std,DiamondValue *out) {
    const size_t rows=normalized->rows,cols=normalized->cols;
    DiamondArray *results=allocate_array(vm,nullptr,0);
    if(results==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(results);

    DiamondTensor *grad_x=allocate_tensor(vm,rows,cols);
    if(grad_x==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!array_push(vm,results,DIAMOND_OBJECT(grad_x)))return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondTensor *grad_gamma=allocate_tensor(vm,1,cols);
    if(grad_gamma==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!array_push(vm,results,DIAMOND_OBJECT(grad_gamma)))return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondTensor *grad_beta=allocate_tensor(vm,1,cols);
    if(grad_beta==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(!array_push(vm,results,DIAMOND_OBJECT(grad_beta)))return DIAMOND_VM_OUT_OF_MEMORY;

    for(size_t row=0;row<rows;row++) {
        const double *dy_row=grad_output->data+row*cols;
        const double *norm_row=normalized->data+row*cols;
        for(size_t col=0;col<cols;col++) {
            const double dy=dy_row[col];
            grad_gamma->data[col]+=dy*norm_row[col];
            grad_beta->data[col]+=dy;
        }
    }
    for(size_t row=0;row<rows;row++) {
        const double *dy_row=grad_output->data+row*cols;
        const double *norm_row=normalized->data+row*cols;
        double *dx_row=grad_x->data+row*cols;
        double dnorm_sum=0.0;
        double dnorm_dot_norm_sum=0.0;
        for(size_t col=0;col<cols;col++) {
            const double dnorm=dy_row[col]*gamma->data[col];
            dnorm_sum+=dnorm;
            dnorm_dot_norm_sum+=dnorm*norm_row[col];
        }
        const double dnorm_mean=dnorm_sum/(double)cols;
        const double dnorm_dot_norm_mean=dnorm_dot_norm_sum/(double)cols;
        const double inv_std_value=inv_std->data[row];
        for(size_t col=0;col<cols;col++) {
            const double dnorm=dy_row[col]*gamma->data[col];
            dx_row[col]=inv_std_value*(dnorm-dnorm_mean-norm_row[col]*dnorm_dot_norm_mean);
        }
    }
    return DIAMOND_VM_OK;
}

/* Tensor#rows/#cols/#get/#set/#matmul/#to_a -- every instance method,
 * matching sqlite3_dispatch_helper's own shape (called from the big
 * receiver_kind==DIAMOND_OBJECT_TENSOR case in run_chunk's INVOKE
 * handling). #get/#set use IndexError for an out-of-bounds row/col,
 * matching Array#[] 's own convention. */
DiamondVmStatus tensor_dispatch_helper(DiamondVm *vm,DiamondTensor *tensor,
        const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base,
        uint8_t argc,uint16_t dest) {
    const bool rows_method=method_name->length==4&&memcmp(method_name->chars,"rows",4)==0;
    const bool cols_method=method_name->length==4&&memcmp(method_name->chars,"cols",4)==0;
    const bool get_method=method_name->length==3&&memcmp(method_name->chars,"get",3)==0;
    const bool set_method=method_name->length==3&&memcmp(method_name->chars,"set",3)==0;
    const bool matmul_method=method_name->length==6&&memcmp(method_name->chars,"matmul",6)==0;
    const bool to_a_method=method_name->length==4&&memcmp(method_name->chars,"to_a",4)==0;
    const bool transpose_method=method_name->length==9&&memcmp(method_name->chars,"transpose",9)==0;
    if(rows_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_INT((int64_t)tensor->rows);return DIAMOND_VM_OK;
    }
    if(cols_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_INT((int64_t)tensor->cols);return DIAMOND_VM_OK;
    }
    if(get_method||set_method) {
        const uint8_t expected_argc=get_method?2:3;
        if(argc!=expected_argc)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "Tensor#%.*s's row/col arguments must be Ints",
                (int)method_name->length,method_name->chars);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t row=registers[base].as.integer;
        const int64_t col=registers[(size_t)base+1].as.integer;
        if(row<0||col<0||(size_t)row>=tensor->rows||(size_t)col>=tensor->cols) {
            snprintf(vm->error,sizeof vm->error,
                "Tensor#%.*s index (%" PRId64 ", %" PRId64 ") out of bounds for %zux%zu Tensor",
                (int)method_name->length,method_name->chars,row,col,tensor->rows,tensor->cols);
            return DIAMOND_VM_INDEX_ERROR;
        }
        if(get_method) {
            registers[dest]=DIAMOND_FLOAT(tensor->data[(size_t)row*tensor->cols+(size_t)col]);
            return DIAMOND_VM_OK;
        }
        const DiamondValue value=registers[(size_t)base+2];
        if(value.kind!=DIAMOND_VALUE_INT&&value.kind!=DIAMOND_VALUE_FLOAT) {
            snprintf(vm->error,sizeof vm->error,"Tensor#set's value argument must be an Int or a Float");
            return DIAMOND_VM_TYPE_ERROR;
        }
        tensor->data[(size_t)row*tensor->cols+(size_t)col]=
            value.kind==DIAMOND_VALUE_INT?(double)value.as.integer:value.as.real;
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(matmul_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_TENSOR) {
            snprintf(vm->error,sizeof vm->error,"Tensor#matmul's argument must be a Tensor");
            return DIAMOND_VM_TYPE_ERROR;
        }
        return tensor_matmul_helper(vm,tensor,
            (const DiamondTensor *)registers[base].as.object,&registers[dest]);
    }
    if(to_a_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        return tensor_to_a_helper(vm,tensor,&registers[dest]);
    }
    if(transpose_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        return tensor_transpose_helper(vm,tensor,&registers[dest]);
    }
    if(method_name->length==4&&memcmp(method_name->chars,"add!",4)==0) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_TENSOR) {
            snprintf(vm->error,sizeof vm->error,"Tensor#add!'s argument must be a Tensor");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondTensor *other=(const DiamondTensor *)registers[base].as.object;
        if(other->rows!=tensor->rows||other->cols!=tensor->cols) {
            snprintf(vm->error,sizeof vm->error,
                "Tensor#add! shape mismatch: %zux%zu + %zux%zu",
                tensor->rows,tensor->cols,other->rows,other->cols);
            return DIAMOND_VM_TYPE_ERROR;
        }
        tensor_add_inplace_helper(tensor,other);
        registers[dest]=DIAMOND_OBJECT(tensor);return DIAMOND_VM_OK;
    }
    if(method_name->length==6&&memcmp(method_name->chars,"scale!",6)==0) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        double scalar=0.0;
        if(!numeric_as_double(registers[base],&scalar)) {
            snprintf(vm->error,sizeof vm->error,"Tensor#scale!'s argument must be an Int or a Float");
            return DIAMOND_VM_TYPE_ERROR;
        }
        tensor_scale_inplace_helper(tensor,scalar);
        registers[dest]=DIAMOND_OBJECT(tensor);return DIAMOND_VM_OK;
    }
    if(method_name->length==9&&memcmp(method_name->chars,"add_bias!",9)==0) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_TENSOR) {
            snprintf(vm->error,sizeof vm->error,"Tensor#add_bias!'s argument must be a Tensor");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondTensor *bias=(const DiamondTensor *)registers[base].as.object;
        if(bias->rows!=1||bias->cols!=tensor->cols) {
            snprintf(vm->error,sizeof vm->error,
                "Tensor#add_bias! expects a 1x%zu bias, got %zux%zu",
                tensor->cols,bias->rows,bias->cols);
            return DIAMOND_VM_TYPE_ERROR;
        }
        tensor_add_bias_inplace_helper(tensor,bias);
        registers[dest]=DIAMOND_OBJECT(tensor);return DIAMOND_VM_OK;
    }
    if(method_name->length==12&&memcmp(method_name->chars,"row_softmax!",12)==0) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        tensor_row_softmax_inplace_helper(tensor);
        registers[dest]=DIAMOND_OBJECT(tensor);return DIAMOND_VM_OK;
    }
    if(method_name->length==5&&memcmp(method_name->chars,"gelu!",5)==0) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        tensor_gelu_inplace_helper(tensor);
        registers[dest]=DIAMOND_OBJECT(tensor);return DIAMOND_VM_OK;
    }
    if(method_name->length==11&&memcmp(method_name->chars,"column_sums",11)==0) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        return tensor_column_sums_helper(vm,tensor,&registers[dest]);
    }
    if(method_name->length==7&&memcmp(method_name->chars,"columns",7)==0) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,"Tensor#columns's arguments must be Ints");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t start=registers[base].as.integer;
        const int64_t width=registers[(size_t)base+1].as.integer;
        if(start<0||width<=0||(size_t)start+(size_t)width>tensor->cols) {
            snprintf(vm->error,sizeof vm->error,
                "Tensor#columns(%" PRId64 ", %" PRId64 ") out of bounds for %zux%zu Tensor",
                start,width,tensor->rows,tensor->cols);
            return DIAMOND_VM_INDEX_ERROR;
        }
        return tensor_columns_helper(vm,tensor,(size_t)start,(size_t)width,&registers[dest]);
    }
    if(method_name->length==12&&memcmp(method_name->chars,"add_columns!",12)==0) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,"Tensor#add_columns!'s first argument must be an Int");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_TENSOR) {
            snprintf(vm->error,sizeof vm->error,"Tensor#add_columns!'s second argument must be a Tensor");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t start=registers[base].as.integer;
        const DiamondTensor *src=(const DiamondTensor *)registers[(size_t)base+1].as.object;
        if(start<0||src->rows!=tensor->rows||(size_t)start+src->cols>tensor->cols) {
            snprintf(vm->error,sizeof vm->error,
                "Tensor#add_columns!(%" PRId64 ", %zux%zu) out of bounds for %zux%zu Tensor",
                start,src->rows,src->cols,tensor->rows,tensor->cols);
            return DIAMOND_VM_INDEX_ERROR;
        }
        tensor_add_columns_inplace_helper(tensor,(size_t)start,src);
        registers[dest]=DIAMOND_OBJECT(tensor);return DIAMOND_VM_OK;
    }
    if(method_name->length==5&&memcmp(method_name->chars,"clone",5)==0) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        DiamondTensor *copy=allocate_tensor(vm,tensor->rows,tensor->cols);
        if(copy==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(copy);
        memcpy(copy->data,tensor->data,tensor->rows*tensor->cols*sizeof(double));
        return DIAMOND_VM_OK;
    }
    /* self is the softmax's own *output* (not the pre-softmax input --
     * see tensor_softmax_backward_helper's own comment). */
    if(method_name->length==16&&memcmp(method_name->chars,"softmax_backward",16)==0) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_TENSOR) {
            snprintf(vm->error,sizeof vm->error,"Tensor#softmax_backward's argument must be a Tensor");
            return DIAMOND_VM_TYPE_ERROR;
        }
        return tensor_softmax_backward_helper(vm,tensor,
            (const DiamondTensor *)registers[base].as.object,&registers[dest]);
    }
    /* self is GELU's original *input* (not its output -- see
     * tensor_gelu_backward_helper's own comment). */
    if(method_name->length==13&&memcmp(method_name->chars,"gelu_backward",13)==0) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_TENSOR) {
            snprintf(vm->error,sizeof vm->error,"Tensor#gelu_backward's argument must be a Tensor");
            return DIAMOND_VM_TYPE_ERROR;
        }
        return tensor_gelu_backward_helper(vm,tensor,
            (const DiamondTensor *)registers[base].as.object,&registers[dest]);
    }
    /* self is x (the layernorm input). (gamma, beta, eps) -> a Diamond
     * Array [output, normalized, inv_std] -- see
     * tensor_layernorm_forward_helper's own comment. */
    if(method_name->length==17&&memcmp(method_name->chars,"layernorm_forward",17)==0) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_TENSOR||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_TENSOR) {
            snprintf(vm->error,sizeof vm->error,
                "Tensor#layernorm_forward's gamma/beta arguments must be Tensors");
            return DIAMOND_VM_TYPE_ERROR;
        }
        double eps=0.0;
        if(!numeric_as_double(registers[(size_t)base+2],&eps)) {
            snprintf(vm->error,sizeof vm->error,
                "Tensor#layernorm_forward's eps argument must be an Int or a Float");
            return DIAMOND_VM_TYPE_ERROR;
        }
        return tensor_layernorm_forward_helper(vm,tensor,
            (const DiamondTensor *)registers[base].as.object,
            (const DiamondTensor *)registers[(size_t)base+1].as.object,eps,&registers[dest]);
    }
    /* self is `normalized` (one of layernorm_forward's own returned
     * values, not x -- the backward formula never needs x directly,
     * only normalized/inv_std, both already cached from forward). Args
     * (gamma, grad_output, inv_std) -> a Diamond Array [grad_x,
     * grad_gamma, grad_beta] -- see
     * tensor_layernorm_backward_helper's own comment. */
    if(method_name->length==18&&memcmp(method_name->chars,"layernorm_backward",18)==0) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_TENSOR||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_TENSOR||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+2].as.object->kind!=DIAMOND_OBJECT_TENSOR) {
            snprintf(vm->error,sizeof vm->error,
                "Tensor#layernorm_backward's arguments must all be Tensors");
            return DIAMOND_VM_TYPE_ERROR;
        }
        return tensor_layernorm_backward_helper(vm,
            (const DiamondTensor *)registers[base].as.object,
            (const DiamondTensor *)registers[(size_t)base+1].as.object,
            tensor,
            (const DiamondTensor *)registers[(size_t)base+2].as.object,&registers[dest]);
    }
    snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
        (int)method_name->length,method_name->chars,"Tensor");
    return DIAMOND_VM_TYPE_ERROR;
}
