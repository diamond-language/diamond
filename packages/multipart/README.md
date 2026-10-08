# multipart

Parse `multipart/form-data` requests and save uploaded files.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add multipart --registry https://cuts.dilang.tech --version "^0.2.1"
facet update
```

This installs the cut into `cuts/multipart/`; load it with `require_cut "multipart"`.

## Usage

`multipart_parse(request)` reads a request Hash from `http` or `gremlin`. This server accepts
an upload and writes the file into an existing `uploads/` directory:

```ruby
# app.di
require_cut "gremlin"
require_cut "multipart"

class UploadsController
  def self.create(request, context)
    upload = multipart_parse(request)
    if upload == nil
      return [400, {"Content-Type": "text/plain"}, "expected multipart/form-data"]
    end
    title = upload["fields"]["title"]
    file = upload["files"]["theme_file"]
    # file => {"filename": "theme.zip", "content_type": "application/zip",
    #          "data": "<raw uploaded bytes>"}
    File.write("uploads/" + file["filename"], file["data"])
    [200, {"Content-Type": "text/plain"}, "saved #{file["filename"]} as #{title}\n"]
  end
end

gremlin_serve(8080, UploadsController.create)
```

```sh
mkdir uploads
diamond app.di
curl -F title=Dark -F theme_file=@theme.zip localhost:8080/
```

Never trust `file["filename"]` as a path: strip directories and validate it before writing,
since it comes straight from the client.

## Notes

`multipart_parse(request)` returns `nil` for a non-multipart request. File entries contain `filename`, `content_type`, and raw `data`.
