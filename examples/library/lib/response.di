# HTML responses go through Div.html_response (packages/div/lib/div/runtime.di)
# directly at each controller call site now -- see README's "Views" section.
class Response
  def self.text(status: Int, body) = [status, {"Content-Type": "text/plain"}, body]
  def self.redirect(location: String, body) = [302, {"Location": location}, body]
  def self.not_found(path) = Response.text(404, "not found: #{path}")
end
