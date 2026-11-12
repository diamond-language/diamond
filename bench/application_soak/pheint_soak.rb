# Derive a bounded-state soak from the existing, semantically checked API driver.
path=ARGV.fetch(0)
source=File.read(path)
def replace!(source,anchor,replacement)
  abort("missing Pheint harness anchor: #{anchor[0,80]}") unless source.scan(anchor).length==1
  source.sub!(anchor,replacement)
end
replace!(source,'["challenge_run", 5],','["write_save", 5],')
replace!(source,'require "./verify"',"require \"./verify\"\nrequire \"./soak_post\"")
replace!(source,'"verified_checks": verified, "scenarios": rows','"verified_checks": verified, "scenarios": rows, "round_samples": loaded, "warmup": warm')
replace!(source,'    verified = verify_workload(world)',<<'D6'.rstrip)
    File.write(ENV["SOAK_PHASE_FILE"], "preflight")
    verified = verify_workload(world)
D6
replace!(source,'    run_id = 0',<<'D6'.rstrip)
    File.write(ENV["SOAK_PHASE_FILE"], "warmup")
    warm = run_load(pool, "mixed", concurrency, (ENV["SOAK_WARMUP_SECONDS"] || "30").to_f(), world, server.pid(), "warm")
    if warm["errors"].length() > 0 then raise "warmup errors: #{JSON.stringify(warm["errors"])}" end
    File.write(ENV["SOAK_RESULT_DIR"] + "/db-warm.json", JSON.stringify(soak_database_state("#{data_dir}/bench.db")))
    run_id = 0
D6
replace!(source,'        idle_run = run_load(pool, name, 1, idle_seconds, world, server.pid(), "#{run_id}i")',<<'D6'.rstrip)
        File.write(ENV["SOAK_PHASE_FILE"], "quiet-#{round + 1}")
        wait_ms((ENV["SOAK_QUIET_SECONDS"] || "10").to_i() * 1000)
        File.write(ENV["SOAK_PHASE_FILE"], "serial-#{round + 1}")
        idle_run = run_load(pool, name, 1, idle_seconds, world, server.pid(), "#{run_id}i")
        File.write(ENV["SOAK_PHASE_FILE"], "load-#{round + 1}")
D6
replace!(source,'    rows = names.map() do |name| summarize(name, loaded[name], idle[name]) end',<<'D6'.rstrip)
    File.write(ENV["SOAK_PHASE_FILE"], "final-quiet")
    wait_ms((ENV["SOAK_FINAL_QUIET_SECONDS"] || "30").to_i() * 1000)
    File.write(ENV["SOAK_PHASE_FILE"], "final-check")
    # Recheck identity, nested data and mutation read-backs after the soak.
    verified += verify_soak_post(world)
    File.write(ENV["SOAK_RESULT_DIR"] + "/db-final.json", JSON.stringify(soak_database_state("#{data_dir}/bench.db")))
    rows = names.map() do |name| summarize(name, loaded[name], idle[name]) end
D6
File.write(path,source)
