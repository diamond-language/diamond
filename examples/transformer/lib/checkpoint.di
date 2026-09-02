# Saves/loads a trained model's weights to/from a plain JSON file --
# every parameter Tensor, in model.parameters()'s own fixed order, each
# as a nested Array of Array of Float (Tensor#to_a/Tensor.from_array).
# Loading replaces each Var's .tensor entirely rather than mutating it
# in place; it does *not* validate that `model` was built with the
# same vocab_size/d_model/etc the checkpoint was saved from -- a
# mismatch surfaces as either a shape error inside Tensor.from_array or
# (worse, silently) wrong-shaped weights, so the caller is responsible
# for reconstructing the model with the exact same config the
# checkpoint was trained with (train_corpus.di prints its config for
# exactly this reason).
class Checkpoint
  def self.save(model, path)
    params = model.parameters()
    data = []
    i = 0
    while i < params.length()
      data.push(params[i].tensor().to_a())
      i += 1
    end
    file = File.open(path, "w")
    file.write(JSON.stringify(data))
    file.close()
    puts("Checkpoint.save: wrote #{params.length()} parameter tensor(s) to #{path}")
  end

  def self.load!(model, path)
    file = File.open(path, "r")
    data = JSON.parse(file.read())
    file.close()
    params = model.parameters()
    if data.length() != params.length()
      raise IOError.new("checkpoint at #{path} has #{data.length()} tensors, model expects #{params.length()} -- built with a different config?")
    end
    i = 0
    while i < params.length()
      params[i].tensor = Tensor.from_array(data[i])
      i += 1
    end
    puts("Checkpoint.load!: loaded #{params.length()} parameter tensor(s) from #{path}")
  end
end
