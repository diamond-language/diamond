# Memoizes one composed chain per class per VM -- the common case (one
# app per process). Each Thread-spawned gremlin worker has its own
# independent VM, so @@instance is independently nil the first time
# each worker's own rack_app runs, and independently set from then on;
# no locking needed even across fibers within one worker, since
# `builder()` does no I/O and can't yield mid-check.
#
# A program that genuinely needs several independent chains at once
# (not the common case this is built for) should write its own small
# memoizing class following this same two-line pattern, keyed however
# it needs -- RackChain itself only ever holds one.
class RackChain
  def self.get(builder: Callable[0])
    if @@instance == nil
      @@instance = builder()
    end
    @@instance
  end
end
