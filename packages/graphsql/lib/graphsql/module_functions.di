module GraphSQL

def self.resolve(relation, db, lookahead, mapping: Mapping,
                 required_associations: Array = [], required_columns: Array = [])
  Resolver.new(relation, db, lookahead, mapping,
    required_associations, required_columns).resolve()
end

end
