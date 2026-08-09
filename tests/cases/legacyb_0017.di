root=TypeError.new("root")
error=RuntimeError.new("wrapped", root)
[error.message(), error.cause().message()]
