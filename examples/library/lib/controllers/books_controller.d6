# Same shape as AuthorsController; see its comments for the action contract.
class BooksController
  # Batch-loads the authors a page of books references, one query
  # instead of one per row -- ActiveRecord::BelongsTo#preload, see
  # packages/active_record/README.md's "N+1" section.
  def self.authors_by_id(db, books: Array)
    author_ids = books.map() do |book| book.author_id() end
    ActiveRecord::BelongsTo.new(Author.repository()).preload(db, author_ids)
  end

  # GET /books: every book, oldest first. Authors are preloaded in one query
  # for the "Author" column.
  def self.index(request, context, params)
    db = Database.get(context)
    books = Book.all().order("year").to_a(db)
    content = books_table_html(books, BooksController.authors_by_id(db, books))
    Div.html_response(200, layout_html("Books", content))
  end

  # GET /books/available: only books whose `available` column is 1.
  def self.available(request, context, params)
    db = Database.get(context)
    books = Book.where({"available": 1}).order("year").to_a(db)
    content = books_table_html(books, BooksController.authors_by_id(db, books))
    Div.html_response(200, layout_html("Available books", content))
  end

  # GET /books/:id.
  def self.show(request, context, params)
    db = Database.get(context)
    id = params["id"].to_i()
    book = Book.find(db, id)

    if book == nil
      return Dials::Response.not_found(request["path"])
    end

    # book.author(db) can be nil -- there's no cascading delete here (see
    # AuthorsController.destroy), so a book can outlive its author.
    author = book.author(db)
    content = book_show_html(id, book.year(), book.available(), author, book.author_id())
    Div.html_response(200, layout_html(book.title(), content))
  end

  # See AuthorsController.form's own comment on why this has to come
  # before new_form/edit, its own callers.
  #
  # Shared "new"/"edit" page; `id` nil means a blank new book. The form also
  # needs every author for its dropdown.
  def self.form(request, context, id)
    db = Database.get(context)
    book = if id == nil
      Book.new({"title": "", "author_id": "", "year": "", "available": 1})
    else
      Book.find(db, id)
    end

    if id != nil && book == nil
      return Dials::Response.not_found(request["path"])
    end

    authors = Author.all().order("name").to_a(db)
    action = if id == nil then "/books" else "/books/#{id}" end
    title = if id == nil then "New book" else "Edit book" end
    submit = if id == nil then "Create book" else "Save book" end

    content = book_form_html(action, authors, book.author_id(), book.title(), book.year(), book.available(), submit)
    Div.html_response(200, layout_html(title, content))
  end

  # Thin wrappers that pick `form`'s id argument (see AuthorsController).
  def self.new_form(request, context, params) = BooksController.form(request, context, nil)
  def self.edit(request, context, params) = BooksController.form(request, context, params["id"].to_i())

  # Form values arrive as strings; the numeric columns are converted so they
  # are stored as integers.
  def self.attributes_from_form(params: Hash)
    {"title": params["title"], "author_id": params["author_id"].to_i(),
     "year": params["year"].to_i(), "available": params["available"].to_i()}
  end

  def self.create(request, context, params)
    db = Database.get(context)
    Book.create(db, BooksController.attributes_from_form(params))
    Dials::Response.redirect("/books", "created")
  end

  def self.update(request, context, params)
    db = Database.get(context)
    id = params["id"].to_i()
    book = Book.find(db, id)

    if book == nil
      return Dials::Response.not_found(request["path"])
    end

    # Copy every editable field from the form onto the loaded record, then
    # save it.
    values = BooksController.attributes_from_form(params)
    book.title = values["title"]
    book.author_id = values["author_id"]
    book.year = values["year"]
    book.available = values["available"]
    book.save(db)
    Dials::Response.redirect("/books/#{id}", "updated")
  end

  # Deleting a missing book is not an error: it just redirects.
  def self.destroy(request, context, params)
    db = Database.get(context)
    id = params["id"].to_i()
    book = Book.find(db, id)

    if book != nil
      book.destroy(db)
    end
    Dials::Response.redirect("/books", "deleted")
  end
end
