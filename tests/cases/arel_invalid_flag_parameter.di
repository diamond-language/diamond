require "../../packages/arel/lib/arel"
Arel::Delete.new(Arel.table("people"), [], [], "allow_all")
