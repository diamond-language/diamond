class HomeController
  def self.index(request, context, params)
    content = home_html(PheintEnvironment.name())
    Div.html_response(200, layout_html("pheint.dia", content))
  end

  def self.health(request, context, params)
    [200, {"Content-Type": "application/json"}, JSON.stringify({
      "status": "ok",
      "application": "pheint.dia",
      "environment": PheintEnvironment.name()
    })]
  end
end
