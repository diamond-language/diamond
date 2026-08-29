# Turns one entry out of multipart_parse's own "files" Hash
# ({"filename", "content_type", "data"}) into a file safely written
# under SkindicateEnvironment.uploads_directory(), returning the
# relative path to store on the Skin record (what StaticFiles later
# serves back from, since public/ -- uploads included -- is all one
# shared StaticFiles root, see boot.di's own comment on why).
#
# The extension check is basic abuse prevention on what a public
# upload endpoint accepts, not a sandbox: StaticFiles never executes
# anything it serves, so this isn't a code-execution concern, just a
# "don't let this become an arbitrary file drop" one. The on-disk name
# is always freshly random (SecureRandom.hex(16) + the original
# extension), never the submitted original filename -- avoiding both
# collisions between two uploads and any path-traversal surface from a
# user-controlled name, the same reasoning packages/rack's own
# StaticFiles rejects a ".." request path for from the read side. The
# real original filename is kept separately (Skin#original_filename)
# for display/download-name purposes; it never touches the filesystem.

def upload_extension(filename)
  reversed = filename.reverse()
  dot_from_end = reversed.index_of(".")
  if dot_from_end == nil
    return nil
  end
  filename.slice(filename.length() - dot_from_end, dot_from_end).downcase()
end

# Returns the stored path relative to public/ (e.g.
# "uploads/abc123....zip") -- already exactly the URL StaticFiles
# serves it back at (with a leading "/") -- on success, or nil if
# `file` is nil (field wasn't submitted) or its extension isn't in
# `allowed_extensions`.
def save_uploaded_file(file, allowed_extensions)
  if file == nil
    return nil
  end
  extension = upload_extension(file["filename"])
  if extension == nil || !allowed_extensions.include?(extension)
    return nil
  end
  stored_name = "#{SecureRandom.hex(16)}.#{extension}"
  full_path = "#{SkindicateEnvironment.uploads_directory()}/#{stored_name}"
  handle = File.open(full_path, "w")
  handle.write(file["data"])
  handle.close()
  "uploads/#{stored_name}"
end
