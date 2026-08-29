def http_status_text(status)
  if status == 200
    "OK"
  elsif status == 201
    "Created"
  elsif status == 204
    "No Content"
  elsif status == 301
    "Moved Permanently"
  elsif status == 302
    "Found"
  elsif status == 400
    "Bad Request"
  elsif status == 401
    "Unauthorized"
  elsif status == 403
    "Forbidden"
  elsif status == 404
    "Not Found"
  elsif status == 405
    "Method Not Allowed"
  elsif status == 500
    "Internal Server Error"
  else
    "Unknown"
  end
end

# A peer's Content-Length header is just a claim -- reading exactly
# that many bytes with no upper bound lets a malicious/misbehaving
# peer declare a multi-gigabyte body and force this "deliberately
# basic" server/client to accumulate that much memory (a request never
# even has to finish; the accumulation itself is the resource cost).
# 25 MiB is generous for the plain-text/JSON bodies this package was
# originally sized for *and* a real uploaded file (packages/multipart
# builds real multipart/form-data parsing on top of this same
# Content-Length-bounded body reading) -- comfortably above a typical
# icon/sound-theme package while still bounding worst-case
# per-connection memory the same way this comment always has
# (http_serve is single-threaded/blocking, so this bounds one
# connection at a time, not a fleet of them). Still a flat, hardcoded
# cap -- no per-route override exists, or is planned; an app needing a
# different limit changes this one shared value.
def http_max_body_size()
  26214400
end
