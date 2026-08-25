class GraphqlController
  def self.json_response(status: Int, value)
    [status, {"Content-Type": "application/json"}, JSON.stringify(value)]
  end

  def self.execute(request, context, params)
    begin
      payload = JSON.parse(request["body"])
    rescue error: JSONError
      return GraphqlController.json_response(400, {
        "errors": [{"message": "request body must be valid JSON"}]
      })
    end
    unless payload is Hash
      return GraphqlController.json_response(400, {
        "errors": [{"message": "request body must be a JSON object"}]
      })
    end
    query = payload["query"]
    unless query is String
      return GraphqlController.json_response(400, {
        "errors": [{"message": "query must be a String"}]
      })
    end
    variables = payload["variables"]
    variables = {} unless variables is Hash
    operation_name = payload["operationName"]
    execution_context = context.merge({
      "request_id": request["request_id"], "request": request
    })
    result = PheintSchema.get().execute(
      query, variables, execution_context, nil, operation_name)
    pheint_log_info(request, context, "graphql.executed", {
      "operation_name": operation_name,
      "has_errors": result["errors"] != nil
    })
    GraphqlController.json_response(200, result)
  end
end
