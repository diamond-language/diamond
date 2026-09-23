module Registry
  class BlobStore
    def self.validate_digest(digest: String)
      if digest.length() != 64 || digest != digest.downcase()
        raise ArgumentError.new("blob digest must be 64 lowercase hexadecimal characters")
      end
      index = 0
      while index < digest.length()
        code = digest[index].ord()
        valid = (code >= 48 && code <= 57) || (code >= 97 && code <= 102)
        unless valid
          raise ArgumentError.new("blob digest must be 64 lowercase hexadecimal characters")
        end
        index += 1
      end
    end

    def initialize(root: String)
      if root.length() == 0 || !File.directory?(root)
        raise ArgumentError.new("blob store root must be an existing directory")
      end
      @root = root
    end

    def path(digest: String) -> String
      BlobStore.validate_digest(digest)
      File.join(@root, digest)
    end

    def contains?(digest: String) -> Bool
      begin
        File.open(self.path(digest), "r").close()
        true
      rescue error: IOError
        false
      end
    end

    # `wx` makes the digest path immutable: a second writer cannot replace an
    # existing archive. The caller must verify bytes and digest before calling
    # this method. A later service layer will add a same-filesystem temporary
    # upload and recovery scan around this primitive.
    def put(digest: String, bytes: String) -> Int
      if Digest.sha256(bytes) != digest
        raise ArgumentError.new("blob bytes do not match digest")
      end
      file = File.open(self.path(digest), "wx")
      file.write(bytes)
      file.close()
      bytes.length()
    end

    def read(digest: String) -> String
      file = File.open(self.path(digest), "r")
      bytes = file.read()
      file.close()
      bytes
    end

  end
end
