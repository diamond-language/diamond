module PheintQueryResolvers
  module_function
  def api_name(object, args, context) = "pheint.dia"
  def environment(object, args, context) = PheintEnvironment.name()
end

class PheintSchema
  def self.get()
    if @@schema == nil
      query = GraphQL::ObjectType.new("Query")
      query.field("apiName", GraphQL::ScalarType.string().non_null(),
        PheintQueryResolvers.api_name)
      query.field("environment", GraphQL::ScalarType.string().non_null(),
        PheintQueryResolvers.environment)
      @@schema = GraphQL::Schema.new().query(query)
    end
    @@schema
  end
end
