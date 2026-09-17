# Tensor#matmul at a size well above DIAMOND_TENSOR_MATMUL_THREAD_FLOOR
# (src/vm.c: 1024*1024 total FLOPS) -- exercises the real threaded,
# k-blocked native matmul, not the single-threaded small-shape fallback
# below that floor. 512x512 * 512x512 is ~268M FLOPS per call, in the
# same ballpark as examples/transformer/benchmark.di's own d_model=512
# projections. No prior bench/*.di coverage existed for Tensor at all --
# only ad-hoc GFLOPS numbers in commit messages -- so this is meant as
# a durable, harness-timed baseline to compare future Tensor/native-VM
# changes against.
def run()
  a = Tensor.random(512, 512, 1)
  b = Tensor.random(512, 512, 2)
  a.matmul(b)
end
run()
