# pipeline: a streaming pipeline whose stages are UNTRUSTED Diamond scripts,
# run under supervision. It combines the sandbox (--sandbox, the
# DIAMOND_SANDBOX_ALLOW allow-list, resource budgets) with Channels, Threads
# and a Supervisor. The plugins example runs one-shot plugins; this one
# keeps a pipeline running while stages misbehave.
#
#   diamond pipeline.di /path/to/diamond [manifest.json]
#
# Shape: items -> normalize -> enrich -> seal -> results, one supervised worker
# per stage, joined by Channels. Each worker runs its stage in a sandboxed
# child process per item. Jobs the sandbox or a budget rejects, or that keep
# crashing, land in a dead-letter channel instead of stopping the pipeline.
require "./lib/worker"

def main(argv)
  if argv.empty?() || argv.length() > 2
    puts("usage: pipeline.di <path-to-diamond-binary> [manifest.json]")
    return 64
  end
  diamond_bin = argv[0]
  manifest_path = argv.length() == 2 ? argv[1] : "manifest.json"
  begin
    manifest = JSON.parse(File.open(manifest_path, "r").read())
  rescue error: IOError
    puts("cannot read #{manifest_path}")
    return 66
  end
  stages = manifest["stages"]
  items = manifest["items"]

  # One channel in front of every stage plus one after the last, a shared
  # dead-letter channel, and one "inflight" slot per stage. Capacities cover
  # the whole batch so no send ever waits.
  capacity = items.length() + 1
  lanes = (0..stages.length()).map() do |_| Channel.new(capacity) end
  dead = Channel.new(capacity * stages.length())
  inflight = stages.map() do |_| Channel.new(1) end

  # One supervised worker per stage. Each runs in its own thread and VM; a
  # crash restarts just that worker, and the channels outlive the restart.
  supervisor = Supervisor.new()
  stages.each_with_index() do |spec, index|
    supervisor.add_child(stage_worker, diamond_bin, spec, manifest["max_attempts"],
                         lanes[index], lanes[index + 1], dead, inflight[index])
  end

  # Feed the items, then close the first lane: end of input.
  items.each_with_index() do |item, index|
    lanes[0].send({"id": index + 1, "item": item, "value": item})
  end
  lanes[0].close()

  # Drain the last lane until the end-of-stream signal travels all the way
  # through (nil), then wait for the workers to finish.
  puts("== results")
  loop do
    done = lanes[stages.length()].receive()
    break if done == nil
    puts("  ##{done["id"]} #{done["item"]} -> #{done["value"]}")
  end
  supervisor.join()

  # Dead letters, in input order (stages run concurrently, so arrival order
  # is not deterministic).
  puts("== dead letters")
  rejected = []
  while dead.size() > 0
    rejected.push(dead.receive())
  end
  rejected.sort_by() do |letter| letter["id"] end.each() do |letter|
    puts("  ##{letter["id"]} #{letter["item"]} at #{letter["stage"]}: #{letter["reason"]} (#{letter["detail"]})")
  end

  # How often each stage's worker was restarted.
  puts("== restarts")
  stages.each_with_index() do |spec, index|
    puts("  #{spec["name"]}: #{supervisor.restart_count(index)}")
  end
  0
end

exit(main(ARGV))
