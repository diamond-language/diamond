# Every action takes (request, context, params): `params` holds the URL's
# `:id` capture and any submitted form fields, both as strings. Each action
# returns a response; page renders go through the compiled div views
# (`*_html` functions), and writes end in a 302 redirect whose body is just
# a short word ("created"/"updated"/"deleted") for clients that do not follow it.
class AuthorsController
  # GET /authors: list everyone, alphabetically.
  def self.index(request, context, params)
    db = Database.get(context)
    authors = Author.all().order("name").to_a(db)
    Div.html_response(200, layout_html("Authors", authors_table_html(authors)))
  end

  # GET /authors/:id: one author and their books, or a 404.
  def self.show(request, context, params)
    db = Database.get(context)
    id = params["id"].to_i()
    author = Author.find(db, id)

    if author == nil
      return Dials::Response.not_found(request["path"])
    end

    content = author_show_html(id, author.name(), author.country(), author.books(db))
    Div.html_response(200, layout_html(author.name(), content))
  end

  # The shared "new" and "edit" page: `id` is nil for a new author (blank
  # form, POSTs to /authors) or an existing id (prefilled, POSTs to
  # /authors/:id).
  def self.form(request, context, id)
    db = Database.get(context)
    author = if id == nil then Author.new({"name": "", "country": ""}) else Author.find(db, id) end

    if id != nil && author == nil
      return Dials::Response.not_found(request["path"])
    end

    # The three strings that differ between creating and editing.
    action = if id == nil then "/authors" else "/authors/#{id}" end
    title = if id == nil then "New author" else "Edit author" end
    submit = if id == nil then "Create author" else "Save author" end

    content = author_form_html(action, author.name(), author.country(), submit)
    Div.html_response(200, layout_html(title, content))
  end

  # GET /authors/new and GET /authors/:id/edit: thin wrappers that pick the
  # `id` argument for `form`.
  def self.new_form(request, context, params) = AuthorsController.form(request, context, nil)
  def self.edit(request, context, params) = AuthorsController.form(request, context, params["id"].to_i())

  # POST /authors: insert, then redirect (so a browser refresh does not
  # resubmit the form).
  def self.create(request, context, params)
    db = Database.get(context)
    Author.create(db, {"name": params["name"], "country": params["country"]})
    Dials::Response.redirect("/authors", "created")
  end

  # POST /authors/:id: load, overwrite the two fields, save.
  def self.update(request, context, params)
    db = Database.get(context)
    id = params["id"].to_i()
    author = Author.find(db, id)

    if author == nil
      return Dials::Response.not_found(request["path"])
    end

    author.name = params["name"]
    author.country = params["country"]
    author.save(db)
    Dials::Response.redirect("/authors/#{id}", "updated")
  end

  # POST /authors/:id/delete. Deleting something already gone is not an
  # error, so a missing author just falls through to the redirect. Their
  # books are NOT deleted and keep a dangling author_id (see
  # BooksController.show).
  def self.destroy(request, context, params)
    db = Database.get(context)
    id = params["id"].to_i()
    author = Author.find(db, id)

    if author != nil
      author.destroy(db)
    end
    Dials::Response.redirect("/authors", "deleted")
  end
end
