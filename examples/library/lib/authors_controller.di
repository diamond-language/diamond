class AuthorsController
  def self.index(request, context)
    db = Database.get(context)
    authors = Author.all().order("name").to_a(db)
    Div.html_response(200, layout_html("Authors", authors_table_html(authors)))
  end

  def self.show(request, context, id)
    db = Database.get(context)
    author = Author.find(db, id)
    if author == nil
      return Response.not_found(request["path"])
    end
    content = author_show_html(id, author.name(), author.country(), author.books(db))
    Div.html_response(200, layout_html(author.name(), content))
  end

  # Diamond's declaration-discovery pass lets one class forward-reference
  # *another* class's not-yet-compiled methods, but a class's own real
  # compile pass rebuilds its method table top to bottom as it goes
  # (see docs/roadmap.md's "Compiler representation" section) -- so
  # `.form` below has to come before new_form/edit, its own callers,
  # even though it's private helper-shaped and would otherwise read
  # better lower down.
  def self.form(request, context, id)
    db = Database.get(context)
    author = if id == nil then Author.new({"name": "", "country": ""}) else Author.find(db, id) end
    if id != nil && author == nil
      return Response.not_found(request["path"])
    end
    action = if id == nil then "/authors" else "/authors/#{id}" end
    title = if id == nil then "New author" else "Edit author" end
    submit = if id == nil then "Create author" else "Save author" end
    content = author_form_html(action, author.name(), author.country(), submit)
    Div.html_response(200, layout_html(title, content))
  end

  def self.new_form(request, context) = AuthorsController.form(request, context, nil)
  def self.edit(request, context, id) = AuthorsController.form(request, context, id)

  def self.create(request, context)
    db = Database.get(context)
    form = Form.parse(request)
    Author.create(db, {"name": form["name"], "country": form["country"]})
    Response.redirect("/authors", "created")
  end

  def self.update(request, context, id)
    db = Database.get(context)
    author = Author.find(db, id)
    if author == nil
      return Response.not_found(request["path"])
    end
    form = Form.parse(request)
    author.name = form["name"]
    author.country = form["country"]
    author.save(db)
    Response.redirect("/authors/#{id}", "updated")
  end

  def self.destroy(request, context, id)
    db = Database.get(context)
    author = Author.find(db, id)
    if author != nil
      author.destroy(db)
    end
    Response.redirect("/authors", "deleted")
  end
end
