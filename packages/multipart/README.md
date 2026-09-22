# multipart

Parse `multipart/form-data` requests and save uploaded files.

## Installation

Install the cut at `cuts/multipart/` and load it with `require_cut "multipart"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## Usage

```ruby
require_cut "multipart"

class UploadsController
  def self.create(request, context, params)
    upload = multipart_parse(request)
    if upload == nil
      return [400, {"Content-Type": "text/plain"}, "expected multipart/form-data"]
    end
    title = upload["fields"]["title"]
    file = upload["files"]["theme_file"]
    # file => {"filename": "theme.zip", "content_type": "application/zip",
    #          "data": "<raw uploaded bytes>"}
    [200, {"Content-Type": "text/plain"}, title]
  end
end
```

## Notes

`multipart_parse(request)` returns `nil` for a non-multipart request. File entries contain `filename`, `content_type`, and raw `data`.
