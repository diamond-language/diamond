class BooksController
  # Batch-loads the authors a page of books references, one query
  # instead of one per row -- ActiveRecord::BelongsTo#preload, see
  # packages/active_record/README.md's "N+1" section.
  def self.authors_by_id(db, books: Array)
    author_ids = books.map() do |book| book.author_id() end
    ActiveRecord::BelongsTo.new(Author.repository()).preload(db, author_ids)
  end

  def self.index(request, context)
    db = Database.get(context)
    books = Book.all().order("year").to_a(db)
    content = books_table_html(books, BooksController.authors_by_id(db, books))
    Div.html_response(200, layout_html("Books", content))
  end

  def self.available(request, context)
    db = Database.get(context)
    books = Book.where({"available": 1}).order("year").to_a(db)
    content = books_table_html(books, BooksController.authors_by_id(db, books))
    Div.html_response(200, layout_html("Available books", content))
  end

  def self.show(request, context, id)
    db = Database.get(context)
    book = Book.find(db, id)
    if book == nil
      return Response.not_found(request["path"])
    end
    # book.author(db) can be nil -- there's no cascading delete here (see
    # AuthorsController.destroy), so a book can outlive its author.
    author = book.author(db)
    content = book_show_html(id, book.year(), book.available(), author, book.author_id())
    Div.html_response(200, layout_html(book.title(), content))
  end

  # See AuthorsController.form's own comment on why this has to come
  # before new_form/edit, its own callers.
  def self.form(request, context, id)
    db = Database.get(context)
    book = if id == nil
      Book.new({"title": "", "author_id": "", "year": "", "available": 1})
    else
      Book.find(db, id)
    end
    if id != nil && book == nil
      return Response.not_found(request["path"])
    end
    authors = Author.all().order("name").to_a(db)
    action = if id == nil then "/books" else "/books/#{id}" end
    title = if id == nil then "New book" else "Edit book" end
    submit = if id == nil then "Create book" else "Save book" end
    content = book_form_html(action, authors, book.author_id(), book.title(), book.year(), book.available(), submit)
    Div.html_response(200, layout_html(title, content))
  end

  def self.new_form(request, context) = BooksController.form(request, context, nil)
  def self.edit(request, context, id) = BooksController.form(request, context, id)

  def self.attributes_from_form(form: Hash)
    {"title": form["title"], "author_id": form["author_id"].to_i(),
     "year": form["year"].to_i(), "available": form["available"].to_i()}
  end

  def self.create(request, context)
    db = Database.get(context)
    Book.create(db, BooksController.attributes_from_form(Form.parse(request)))
    Response.redirect("/books", "created")
  end

  def self.update(request, context, id)
    db = Database.get(context)
    book = Book.find(db, id)
    if book == nil
      return Response.not_found(request["path"])
    end
    values = BooksController.attributes_from_form(Form.parse(request))
    book.title = values["title"]
    book.author_id = values["author_id"]
    book.year = values["year"]
    book.available = values["available"]
    book.save(db)
    Response.redirect("/books/#{id}", "updated")
  end

  def self.destroy(request, context, id)
    db = Database.get(context)
    book = Book.find(db, id)
    if book != nil
      book.destroy(db)
    end
    Response.redirect("/books", "deleted")
  end
end
