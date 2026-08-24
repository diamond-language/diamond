class Router
  def self.path_without_query(path)
    question = path.index_of("?")
    if question == nil then path else path.slice(0, question) end
  end

  # "42/edit" -> [42, "/edit"]; "42" -> [42, ""].
  def self.segment_id(rest: String)
    slash = rest.index_of("/")
    id_text = if slash == nil then rest else rest.slice(0, slash) end
    suffix = if slash == nil then "" else rest.slice(slash, rest.length()) end
    [id_text.to_i(), suffix]
  end

  def self.dispatch(request, context)
    path = Router.path_without_query(request["path"])
    if path == "/"
      Div.html_response(200, layout_html("Library", home_html()))
    elsif path == "/authors"
      if request["method"] == "POST"
        AuthorsController.create(request, context)
      else
        AuthorsController.index(request, context)
      end
    elsif path == "/authors/new"
      AuthorsController.new_form(request, context)
    elsif path == "/books"
      if request["method"] == "POST"
        BooksController.create(request, context)
      else
        BooksController.index(request, context)
      end
    elsif path == "/books/new"
      BooksController.new_form(request, context)
    elsif path == "/books/available"
      BooksController.available(request, context)
    elsif path.slice(0, 9) == "/authors/"
      id, suffix = Router.segment_id(path.slice(9, path.length()))
      if suffix == "/edit"
        AuthorsController.edit(request, context, id)
      elsif suffix == "/delete"
        AuthorsController.destroy(request, context, id)
      elsif request["method"] == "POST"
        AuthorsController.update(request, context, id)
      else
        AuthorsController.show(request, context, id)
      end
    elsif path.slice(0, 7) == "/books/"
      id, suffix = Router.segment_id(path.slice(7, path.length()))
      if suffix == "/edit"
        BooksController.edit(request, context, id)
      elsif suffix == "/delete"
        BooksController.destroy(request, context, id)
      elsif request["method"] == "POST"
        BooksController.update(request, context, id)
      else
        BooksController.show(request, context, id)
      end
    else
      Response.not_found(request["path"])
    end
  end
end
