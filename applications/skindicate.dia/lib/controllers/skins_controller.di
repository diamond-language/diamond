class SkinsController
  def self.newest_first(relation) = relation.order(Arel.table("skins").column("id").desc())

  def self.index(request, context, params)
    db = Database.get(context)
    skins = SkinsController.newest_first(Skin.all()).to_a(db)
    log_debug(request, context, "skins.listed", {"count": skins.length()})
    Div.html_response(200, layout_html("Browse skins", skins_index_html(skins, db), context["current_user"], context["csrf_token"]))
  end

  def self.by_tag(request, context, params)
    db = Database.get(context)
    tag = ActiveTagging::Tag.where({"name": params["name"]}).first(db)
    skins = if tag == nil
      []
    else
      skin_ids = ActiveTagging::Tagging.taggable_ids_for_tag(db, tag.id())
      matched = []
      closure collect_skin(id)
        matched.push(Skin.find(db, id))
      end
      skin_ids.each(collect_skin)
      matched
    end
    log_debug(request, context, "skins.listed_by_tag", {"tag": params["name"], "count": skins.length()})
    Div.html_response(200, layout_html("Tagged: #{params["name"]}", skins_index_html(skins, db), context["current_user"], context["csrf_token"]))
  end

  def self.show(request, context, params)
    db = Database.get(context)
    skin = Skin.find(db, params["id"].to_i())
    if skin == nil then return Dials::Response.not_found(request["path"]) end
    skin_entry = skin.entry(db)
    uploader = User.find(db, skin_entry.user_id())
    tags = skin.tags(db)
    comment_entries = skin_entry.children(db)
    is_owner = context["current_user"] != nil && context["current_user"].id() == skin_entry.user_id()
    log_debug(request, context, "skin.shown", {"skin_id": skin.id(), "comment_count": comment_entries.length()})
    content = skin_show_html(skin, uploader, tags, is_owner, context["current_user"], skin_entry, comment_entries, db, context["csrf_token"])
    Div.html_response(200, layout_html(skin.title(), content, context["current_user"], context["csrf_token"]))
  end

  def self.render_form(request, context, skin, tags_text, id, errors, status)
    action = if id == nil then "/skins" else "/skins/#{id}" end
    submit = if id == nil then "Submit skin" else "Save skin" end
    page_title = if id == nil then "Submit a skin" else "Edit skin" end
    content = skin_form_html(action, skin.title(), skin.description(), skin.platform(), tags_text, submit, context["csrf_token"], errors, skin_platforms())
    Div.html_response(status, layout_html(page_title, content, context["current_user"], context["csrf_token"]))
  end

  def self.new_form(request, context, params)
    SkinsController.render_form(request, context, Skin.new({"title": "", "description": "", "platform": ""}), "", nil, [], 200)
  end

  def self.edit(request, context, params)
    skin = context["current_skin"]
    tag_names = []
    closure collect_tag_name(tag)
      tag_names.push(tag.name())
    end
    skin.tags(Database.get(context)).each(collect_tag_name)
    SkinsController.render_form(request, context, skin, tag_names.join(", "), skin.id(), [], 200)
  end

  def self.create(request, context, params)
    upload = multipart_upload(request, context)
    if upload == nil
      return Dials::Response.text(400, "expected multipart/form-data")
    end
    fields = upload["fields"]
    db = Database.get(context)
    theme_file = upload["files"]["theme_file"]
    theme_path = skindicate_save_upload(theme_file, ["zip", "itheme", "deskthemepack"])
    preview_path = skindicate_save_upload(upload["files"]["preview_image"], ["png", "jpg", "jpeg", "gif", "webp"])
    original_filename = if theme_file == nil then nil else theme_file["filename"] end
    skin = Skin.new({"title": fields["title"], "description": fields["description"], "platform": fields["platform"], "preview_image_path": preview_path, "file_path": theme_path, "original_filename": original_filename})
    begin
      create_entry!(db, "Skin", skin, context["current_user"].id())
    rescue error: ActiveRecord::ValidationError
      log_warn(request, context, "skin.create_rejected", {"validation_errors": error.errors()})
      return SkinsController.render_form(request, context, skin, fields["tags"], nil, error.errors(), 422)
    end
    ActiveTagging::Tagging.set_tags(db, skin.id(), ActiveTagging::Tag.parse_names(fields["tags"]))
    log_info(request, context, "skin.created", {"skin_id": skin.id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/skins/#{skin.id()}", "created")
  end

  def self.update(request, context, params)
    skin = context["current_skin"]
    upload = multipart_upload(request, context)
    if upload == nil
      return Dials::Response.text(400, "expected multipart/form-data")
    end
    fields = upload["fields"]
    db = Database.get(context)
    new_theme_file = upload["files"]["theme_file"]
    new_theme_path = skindicate_save_upload(new_theme_file, ["zip", "itheme", "deskthemepack"])
    new_preview_path = skindicate_save_upload(upload["files"]["preview_image"], ["png", "jpg", "jpeg", "gif", "webp"])
    skin.title = fields["title"]
    skin.description = fields["description"]
    skin.platform = fields["platform"]
    if new_theme_path != nil
      skin.file_path = new_theme_path
      skin.original_filename = new_theme_file["filename"]
    end
    if new_preview_path != nil
      skin.preview_image_path = new_preview_path
    end
    begin
      skin.save(db)
    rescue error: ActiveRecord::ValidationError
      log_warn(request, context, "skin.update_rejected", {"skin_id": skin.id(), "validation_errors": error.errors()})
      return SkinsController.render_form(request, context, skin, fields["tags"], skin.id(), error.errors(), 422)
    end
    ActiveTagging::Tagging.set_tags(db, skin.id(), ActiveTagging::Tag.parse_names(fields["tags"]))
    log_info(request, context, "skin.updated", {"skin_id": skin.id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/skins/#{skin.id()}", "updated")
  end

  def self.destroy(request, context, params)
    db = Database.get(context)
    skin = context["current_skin"]
    skin_id = skin.id()
    # No FOREIGN KEY ... ON DELETE CASCADE from taggings.taggable_id --
    # unlike the old skin_id column, it's opaque to the taggings table
    # now (see setup_db.di's own comment), so this has to be explicit,
    # matching destroy_subtree! below's own explicit (not FK-driven)
    # cleanup of the entry/comment subtree.
    ActiveTagging::Tagging.set_tags(db, skin_id, [])
    skin.entry(db).destroy_subtree!(db)
    log_info(request, context, "skin.deleted", {"skin_id": skin_id, "user_id": context["current_user"].id()})
    Dials::Response.redirect("/", "deleted")
  end
end
