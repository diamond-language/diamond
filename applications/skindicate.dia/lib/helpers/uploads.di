# Thin app-specific wrapper around packages/multipart's own
# multipart_save_file: this app serves everything under public/ (see
# boot.di's own comment on why uploads and the framework's static
# assets share one StaticFiles root) at a URL matching the file's path
# relative to public/, so the stored value needs an "uploads/" prefix
# -- a URL-scheme choice this app owns, not something the generic
# packages/multipart helper should assume.
def skindicate_save_upload(file, allowed_extensions)
  stored_name = multipart_save_file(file, allowed_extensions, SkindicateEnvironment.uploads_directory())
  if stored_name == nil then nil else "uploads/#{stored_name}" end
end
