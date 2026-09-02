module GraphSQL

  class UnknownColumnError < StandardError
    def self.for_mapped_column(type_name, field, column, model_name)
      UnknownColumnError.new(
        "#{type_name}'s column mapping #{field} maps to '#{column}', but #{model_name} has no such column")
    end

    def self.for_required_column(column, model_name)
      UnknownColumnError.new(
        "required_columns referenced '#{column}', but #{model_name} has no such column")
    end
  end

end
