# A `closure` declared where no `self` exists (top level here) is a
# compile-time error, not a runtime one -- see closure_self_capture.di
# for the feature itself.
closure inner()
  1
end
