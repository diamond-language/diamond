# ProgramBuilder#expand_source resolves `require` by reading files and returns the
# expanded text, so it is a file read. Under the sandbox it must be denied like
# File.read is, or a script could read any file whose name ends in .di.
begin
  ProgramBuilder.new().expand_source("main.di", "require \"/etc/hostname\"\n")
  "escaped"
rescue error: SandboxError
  error.message()
end
