require "./boot"

if SkindicateEnvironment.name() != "test"
  raise "smoke_test.di requires DIAMOND_ENV=test"
end

def request(method, path, body = "", cookie = nil)
  headers = {}
  if cookie != nil
    headers["cookie"] = cookie
  end
  {"method": method, "path": path, "body": body, "headers": headers}
end

def request_with_cookie(method, path, body, cookie) = request(method, path, body, cookie)

def multipart_request(method, path, fields, files, cookie)
  boundary = "----TestBoundary#{SecureRandom.hex(8)}"
  parts = []
  fields.each() do |name, value|
    parts.push("--#{boundary}\r\nContent-Disposition: form-data; name=\"#{name}\"\r\n\r\n#{value}")
  end
  files.each() do |name, file|
    parts.push("--#{boundary}\r\nContent-Disposition: form-data; name=\"#{name}\"; filename=\"#{file["filename"]}\"\r\nContent-Type: #{file["content_type"]}\r\n\r\n#{file["data"]}")
  end
  body = parts.join("\r\n") + "\r\n--#{boundary}--\r\n"
  headers = {"content-type": "multipart/form-data; boundary=#{boundary}"}
  if cookie != nil
    headers["cookie"] = cookie
  end
  {"method": method, "path": path, "headers": headers, "body": body}
end

context = {}

# --- anonymous browsing ---
public_index = app(request("GET", "/"), context)
if public_index[0] != 200 then raise "public skins index failed" end

# --- signup ---
signup_body = "email=user1%40example.com&username=user1&password=hunter22"
signup = app(request("POST", "/signup", signup_body), context)
if signup[0] != 302 || signup[1]["Set-Cookie"] == nil then raise "signup failed" end
cookie = signup[1]["Set-Cookie"]
csrf_token = context["csrf_token"]
if csrf_token == nil then raise "signup did not issue a CSRF token" end

# --- duplicate signup is rejected with validation errors ---
duplicate_signup = app(request("POST", "/signup", signup_body), context)
if duplicate_signup[0] != 422 || !duplicate_signup[2].include?("already been taken")
  raise "duplicate signup was not rejected"
end

# --- login: wrong password rejected, right password accepted ---
bad_login = app(request("POST", "/login", "email=user1%40example.com&password=wrong"), context)
if bad_login[0] != 401 then raise "bad login was not rejected" end

login = app(request("POST", "/login", "email=user1%40example.com&password=hunter22"), context)
if login[0] != 302 || login[1]["Set-Cookie"] == nil then raise "login failed" end
cookie = login[1]["Set-Cookie"]
csrf_token = context["csrf_token"]

# --- a forged/tampered session cookie is treated as logged out, no crash ---
forged = app(request("GET", "/skins/new", "", "session_token=not-a-real-signed-value"), context)
if forged[0] != 302 || forged[1]["Location"] != "/login" then raise "forged cookie was not rejected cleanly" end

# --- the submission form renders the real CSRF token ---
form = app(request_with_cookie("GET", "/skins/new", "", cookie), context)
if form[0] != 200 || !form[2].include?(csrf_token) then raise "submission form did not render its CSRF token" end

theme_file = {"filename": "theme.zip", "content_type": "application/zip", "data": "PK-fake-zip-bytes"}

# --- CSRF: missing and wrong tokens are rejected ---
no_csrf = app(multipart_request("POST", "/skins", {"title": "T", "description": "D", "platform": "windows", "tags": ""}, {"theme_file": theme_file}, cookie), context)
if no_csrf[0] != 403 then raise "skin creation without a CSRF token was not rejected" end

wrong_csrf = app(multipart_request("POST", "/skins", {"title": "T", "description": "D", "platform": "windows", "tags": "", "csrf_token": "wrong"}, {"theme_file": theme_file}, cookie), context)
if wrong_csrf[0] != 403 then raise "skin creation with a wrong CSRF token was not rejected" end

# --- a missing theme file is a validation error, not a crash ---
no_file = app(multipart_request("POST", "/skins", {"title": "T", "description": "D", "platform": "windows", "tags": "", "csrf_token": csrf_token}, {}, cookie), context)
if no_file[0] != 422 then raise "skin creation without a theme file was not rejected" end

# --- a real multipart submission with the correct CSRF token succeeds,
# --- the uploaded file actually lands on disk, and tags are created ---
create = app(multipart_request("POST", "/skins", {"title": "Midnight Blue", "description": "A dark taskbar theme.", "platform": "windows", "tags": "Dark Mode, Minimal", "csrf_token": csrf_token}, {"theme_file": theme_file, "preview_image": {"filename": "preview.png", "content_type": "image/png", "data": "fake-png-bytes"}}, cookie), context)
if create[0] != 302 then raise "skin creation failed" end

skin = Skin.where({"title": "Midnight Blue"}).first(Database.get(context))
if skin == nil then raise "created skin could not be reloaded" end
if skin.file_path() == nil || !File.open("public/#{skin.file_path()}", "r").read().include?("PK-fake-zip-bytes")
  raise "uploaded theme file was not written to disk correctly"
end

show = app(request("GET", "/skins/#{skin.id()}"), context)
if show[0] != 200 || !show[2].include?("dark-mode") || !show[2].include?("minimal")
  raise "skin detail page did not render its tags"
end

by_tag = app(request("GET", "/tags/dark-mode"), context)
if by_tag[0] != 200 || !by_tag[2].include?("Midnight Blue") then raise "/tags/:name filtering failed" end

# --- a second skin reusing an existing tag doesn't create a duplicate ---
second_create = app(multipart_request("POST", "/skins", {"title": "Second Skin", "description": "Another one.", "platform": "macos", "tags": "dark-mode", "csrf_token": csrf_token}, {"theme_file": theme_file}, cookie), context)
if second_create[0] != 302 then raise "second skin creation failed" end
if Tag.where({"name": "dark-mode"}).to_a(Database.get(context)).length() != 1
  raise "an existing tag was duplicated instead of reused"
end
by_tag_again = app(request("GET", "/tags/dark-mode"), context)
if !by_tag_again[2].include?("Midnight Blue") || !by_tag_again[2].include?("Second Skin")
  raise "a reused tag did not list both of its skins"
end

# --- ownership: a second user cannot edit or delete the first user's
# --- skin -- 404, not 403, so existence isn't leaked ---
app(request("POST", "/signup", "email=user2%40example.com&username=user2&password=hunter22"), context)
other_login = app(request("POST", "/login", "email=user2%40example.com&password=hunter22"), context)
other_cookie = other_login[1]["Set-Cookie"]
other_csrf = context["csrf_token"]

not_owner_edit = app(request_with_cookie("GET", "/skins/#{skin.id()}/edit", "", other_cookie), context)
if not_owner_edit[0] != 404 then raise "a non-owner was allowed to view another user's edit form" end

not_owner_delete = app(multipart_request("POST", "/skins/#{skin.id()}/delete", {"csrf_token": other_csrf}, {}, other_cookie), context)
if not_owner_delete[0] != 404 then raise "a non-owner was allowed to delete another user's skin" end
if Skin.find(Database.get(context), skin.id()) == nil then raise "a rejected delete somehow removed the skin" end

# --- comments/replies/report/moderation, factored into its own
# --- function purely to keep this script's own flat top-level local
# --- count under DIAMOND_MAX_LOCALS (64) -- no other reason for the
# --- split. `skin` here is "Midnight Blue", owned by `cookie`/
# --- `csrf_token`'s user (user1); `other_cookie`/`other_csrf` is
# --- user2. ---
def test_discussion_features(context, skin, cookie, csrf_token, other_cookie, other_csrf)
  db = Database.get(context)
  # --- any signed-in user can post, and it renders; the comment's own
  # --- author *or* the skin's owner (moderating their own discussion)
  # --- can delete it; a genuine third party cannot (404, same
  # --- not-403 shape) ---
  comment_post = app(request_with_cookie("POST", "/skins/#{skin.id()}/comments", "csrf_token=#{other_csrf}&body=Great+theme", other_cookie), context)
  if comment_post[0] != 302 then raise "comment creation failed" end
  show_with_comment = app(request("GET", "/skins/#{skin.id()}"), context)
  if !show_with_comment[2].include?("Great theme") || !show_with_comment[2].include?("user2")
    raise "posted comment did not render with its author"
  end
  comments_in_db = discussion_for_skin(db, skin.id()).top_level_comments(db)
  if comments_in_db.length() != 1 then raise "expected exactly one comment on the skin" end
  comment_id = comments_in_db[0].id()

  app(request("POST", "/signup", "email=user5%40example.com&username=user5&password=hunter22"), context)
  third_party_login = app(request("POST", "/login", "email=user5%40example.com&password=hunter22"), context)
  third_party_cookie = third_party_login[1]["Set-Cookie"]
  third_party_csrf = context["csrf_token"]

  not_author_delete = app(request_with_cookie("POST", "/comments/#{comment_id}/delete", "csrf_token=#{third_party_csrf}", third_party_cookie), context)
  if not_author_delete[0] != 404 then raise "a genuine third party was allowed to delete another user's comment" end
  if ActiveDiscussion::Comment.find(db, comment_id) == nil then raise "a rejected comment delete somehow removed it" end

  owner_moderate_delete = app(request_with_cookie("POST", "/comments/#{comment_id}/delete", "csrf_token=#{csrf_token}", cookie), context)
  if owner_moderate_delete[0] != 302 || ActiveDiscussion::Comment.find(db, comment_id) != nil
    raise "the skin owner could not moderate-delete another user's comment on their own skin"
  end

  second_comment_post = app(request_with_cookie("POST", "/skins/#{skin.id()}/comments", "csrf_token=#{other_csrf}&body=Another+one", other_cookie), context)
  if second_comment_post[0] != 302 then raise "second comment creation failed" end
  second_comment_id = discussion_for_skin(db, skin.id()).top_level_comments(db)[0].id()
  author_delete = app(request_with_cookie("POST", "/comments/#{second_comment_id}/delete", "csrf_token=#{other_csrf}", other_cookie), context)
  if author_delete[0] != 302 || ActiveDiscussion::Comment.find(db, second_comment_id) != nil
    raise "the comment's own author could not delete it"
  end

  # --- replies: threading one level deep, and a reply beyond
  # --- max_depth (this discussion is capped at 1) is rejected ---
  top_comment_post = app(request_with_cookie("POST", "/skins/#{skin.id()}/comments", "csrf_token=#{other_csrf}&body=Top+level", other_cookie), context)
  if top_comment_post[0] != 302 then raise "top-level comment creation for reply test failed" end
  top_comment = discussion_for_skin(db, skin.id()).top_level_comments(db)[0]

  reply_post = app(request_with_cookie("POST", "/skins/#{skin.id()}/comments", "csrf_token=#{csrf_token}&body=A+reply&parent_id=#{top_comment.id()}", cookie), context)
  if reply_post[0] != 302 then raise "reply creation failed" end
  show_with_reply = app(request("GET", "/skins/#{skin.id()}"), context)
  if !show_with_reply[2].include?("comment-reply") || !show_with_reply[2].include?("A reply")
    raise "reply did not render as a nested comment-reply"
  end
  reply = top_comment.replies(db)[0]

  too_deep_reply = app(request_with_cookie("POST", "/skins/#{skin.id()}/comments", "csrf_token=#{other_csrf}&body=Too+deep&parent_id=#{reply.id()}", other_cookie), context)
  if too_deep_reply[0] != 302 then raise "a rejected reply-to-a-reply should still redirect, not error" end
  if reply.replies(db).length() != 0
    raise "a reply beyond this discussion's max_depth of 1 was not rejected"
  end

  # --- report: creates a flame signal against the comment's author,
  # --- discoverable via the package's own API ---
  report = app(request_with_cookie("POST", "/comments/#{top_comment.id()}/report", "csrf_token=#{csrf_token}", cookie), context)
  if report[0] != 302 then raise "report request failed" end
  if discussion_for_skin(db, skin.id()).signals_for(db).length() != 1
    raise "reporting a comment did not create a flame signal"
  end

  # --- moderation: the skin owner can lock the discussion (rejecting
  # --- new comments) and unlock it again ---
  lock = app(request_with_cookie("POST", "/skins/#{skin.id()}/lock", "csrf_token=#{csrf_token}", cookie), context)
  if lock[0] != 302 then raise "discussion lock request failed" end
  locked_show = app(request("GET", "/skins/#{skin.id()}"), context)
  if !locked_show[2].include?("Discussion locked") then raise "locked discussion did not render its notice" end

  rejected_while_locked = app(request_with_cookie("POST", "/skins/#{skin.id()}/comments", "csrf_token=#{other_csrf}&body=Blocked", other_cookie), context)
  if rejected_while_locked[0] != 302 then raise "a rejected comment while locked should still redirect, not error" end
  comment_count_while_locked = discussion_for_skin(db, skin.id()).comment_count(db)

  unlock = app(request_with_cookie("POST", "/skins/#{skin.id()}/unlock", "csrf_token=#{csrf_token}", cookie), context)
  if unlock[0] != 302 then raise "discussion unlock request failed" end
  allowed_after_unlock = app(request_with_cookie("POST", "/skins/#{skin.id()}/comments", "csrf_token=#{other_csrf}&body=Allowed+again", other_cookie), context)
  if allowed_after_unlock[0] != 302 then raise "unlocking the discussion failed" end
  if discussion_for_skin(db, skin.id()).comment_count(db) != comment_count_while_locked + 1
    raise "a comment posted after unlocking was not actually admitted"
  end
end

test_discussion_features(context, skin, cookie, csrf_token, other_cookie, other_csrf)

# --- the real owner can edit (keeping the existing file) and delete ---
owner_edit_form = app(request_with_cookie("GET", "/skins/#{skin.id()}/edit", "", cookie), context)
if owner_edit_form[0] != 200 || !owner_edit_form[2].include?("dark-mode") || !owner_edit_form[2].include?("minimal")
  raise "owner edit form did not pre-fill existing tags"
end

update = app(multipart_request("POST", "/skins/#{skin.id()}", {"title": "Midnight Blue v2", "description": "Updated.", "platform": "windows", "tags": "dark-mode", "csrf_token": csrf_token}, {}, cookie), context)
if update[0] != 302 then raise "owner update failed" end
updated_skin = Skin.find(Database.get(context), skin.id())
if updated_skin.title() != "Midnight Blue v2" || updated_skin.file_path() != skin.file_path()
  raise "owner update did not persist, or dropped the existing file when none was resubmitted"
end

delete = app(multipart_request("POST", "/skins/#{skin.id()}/delete", {"csrf_token": csrf_token}, {}, cookie), context)
if delete[0] != 302 || Skin.find(Database.get(context), skin.id()) != nil
  raise "owner delete failed"
end

# --- logout clears the server-side session row and the cookie ---
logout = app(multipart_request("POST", "/logout", {"csrf_token": csrf_token}, {}, cookie), context)
if logout[0] != 302 || logout[1]["Set-Cookie"] == nil || !logout[1]["Set-Cookie"].include?("Max-Age=0")
  raise "logout did not clear the session cookie"
end
denied_after_logout = app(request_with_cookie("GET", "/skins/new", "", cookie), context)
if denied_after_logout[0] != 302 || denied_after_logout[1]["Location"] != "/login"
  raise "a destroyed session still authorized a protected route"
end

# --- an expired session is treated as logged out and the stale cookie
# --- is cleared, mirroring examples/project_board's own convention ---
relogin = app(request("POST", "/login", "email=user1%40example.com&password=hunter22"), context)
relogin_cookie = relogin[1]["Set-Cookie"]
form_again = app(request_with_cookie("GET", "/skins/new", "", relogin_cookie), context)
if form_again[0] != 200 || context["current_session"] == nil then raise "second session was not loaded" end
session_id = context["current_session"].id()
Database.get(context).execute("UPDATE sessions SET expires_at = ? WHERE id = ?", [Time.now().to_i() - 1, session_id])
expired = app(request_with_cookie("GET", "/skins/new", "", relogin_cookie), context)
if expired[0] != 302 || expired[1]["Location"] != "/login" then raise "expired session still authorized a protected route" end
if expired[1]["Set-Cookie"] == nil || !expired[1]["Set-Cookie"].include?("Max-Age=0")
  raise "expired session cookie was not cleared"
end
if Session.find(Database.get(context), session_id) != nil then raise "expired session was not deleted" end

# --- following: a signed-up user can view another's profile, follow
# --- and unfollow them, and the counts/button state track it ---
app(request("POST", "/signup", "email=user3%40example.com&username=user3&password=hunter22"), context)
follower_login = app(request("POST", "/login", "email=user3%40example.com&password=hunter22"), context)
follower_cookie = follower_login[1]["Set-Cookie"]
follower_csrf = context["csrf_token"]

app(request("POST", "/signup", "email=user4%40example.com&username=user4&password=hunter22"), context)

anon_profile = app(request("GET", "/users/user4"), context)
if anon_profile[0] != 200 || anon_profile[2].include?("Follow<") then raise "anonymous profile view failed or showed a follow button" end

# --- the embedded skin grid renders as real HTML, not escaped text
# --- (user1 still owns "Second Skin" from earlier in this test) ---
user1_profile = app(request("GET", "/users/user1"), context)
if user1_profile[0] != 200 || !user1_profile[2].include?("class=\"skin-card\"") || !user1_profile[2].include?("Second Skin")
  raise "profile page's skin grid was escaped instead of rendered as HTML"
end

missing_profile = app(request("GET", "/users/no-such-user"), context)
if missing_profile[0] != 404 then raise "a missing user's profile did not 404" end

own_profile = app(request_with_cookie("GET", "/users/user3", "", follower_cookie), context)
if own_profile[0] != 200 || own_profile[2].include?("Follow<") || own_profile[2].include?("Unfollow<")
  raise "a user's own profile showed a follow/unfollow button"
end

before_follow = app(request_with_cookie("GET", "/users/user4", "", follower_cookie), context)
if before_follow[0] != 200 || !before_follow[2].include?("Follow<") ||
   !before_follow[2].include?("<strong>0</strong> following") || !before_follow[2].include?("<strong>0</strong> followers")
  raise "target profile did not show a Follow button with zero counts before following"
end

follow = app(request_with_cookie("POST", "/users/user4/follow", "csrf_token=#{follower_csrf}", follower_cookie), context)
if follow[0] != 302 then raise "follow request failed" end

after_follow = app(request_with_cookie("GET", "/users/user4", "", follower_cookie), context)
if after_follow[0] != 200 || !after_follow[2].include?("Unfollow<") ||
   !after_follow[2].include?("<strong>0</strong> following") || !after_follow[2].include?("<strong>1</strong> followers")
  raise "target profile did not reflect the new follower"
end
follower_own_profile = app(request_with_cookie("GET", "/users/user3", "", follower_cookie), context)
if !follower_own_profile[2].include?("<strong>1</strong> following") || !follower_own_profile[2].include?("<strong>0</strong> followers")
  raise "follower's own profile did not reflect their new following count"
end

follow_again = app(request_with_cookie("POST", "/users/user4/follow", "csrf_token=#{follower_csrf}", follower_cookie), context)
if follow_again[0] != 302 then raise "idempotent re-follow request failed" end
if ActiveSocial::Follow.followers_count(Database.get(context), User.where({"username": "user4"}).first(Database.get(context)).id()) != 1
  raise "re-following the same user created a duplicate follow row"
end

unfollow = app(request_with_cookie("POST", "/users/user4/unfollow", "csrf_token=#{follower_csrf}", follower_cookie), context)
if unfollow[0] != 302 then raise "unfollow request failed" end
after_unfollow = app(request_with_cookie("GET", "/users/user4", "", follower_cookie), context)
if after_unfollow[0] != 200 || !after_unfollow[2].include?("Follow<") ||
   !after_unfollow[2].include?("<strong>0</strong> following") || !after_unfollow[2].include?("<strong>0</strong> followers")
  raise "target profile did not reflect the unfollow"
end

JSON.stringify({"timestamp": Time.now().strftime("%Y-%m-%dT%H:%M:%S%z"), "level": "info", "tag": "skindicate_smoke_test", "message": "smoke_test.passed"})
