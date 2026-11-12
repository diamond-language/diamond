# Identical measurement-only overlay for RC3 and the candidate. Gremlin exits
# directly on shutdown, so observe actual GC checkpoints rather than teardown.
path=File.join(ARGV.fetch(0),'src/vm_gc.c')
source=File.read(path)
%w[major minor].each do |kind|
  anchor="    vm->gc_#{kind}_collection_count++;\n"
  abort("unexpected #{kind} GC counter") unless source.scan(anchor).length==1
  source.sub!(anchor,anchor+<<C)
    if((vm->gc_#{kind}_collection_count==1||vm->gc_#{kind}_collection_count%256==0)&&
       getenv("SOAK_DIAGNOSTICS")!=nullptr)
        fprintf(stderr,"SOAK_GC kind=#{kind} seconds=%.6f thread=%lu major=%zu minor=%zu jit=%zu live_bytes=%zu\\n",
            (double)end.tv_sec+(double)end.tv_nsec/1e9,(unsigned long)pthread_self(),
            vm->gc_major_collection_count,vm->gc_minor_collection_count,
            vm->jit_compiled_functions,vm->bytes_allocated);
C
end
File.write(path,source)
