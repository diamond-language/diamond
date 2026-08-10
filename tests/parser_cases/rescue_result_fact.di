def recover(flag: Bool) -> Int
  begin
    if flag
      raise "boom"
    else
      40
    end
  rescue
    42
  end
end

recover(true)
