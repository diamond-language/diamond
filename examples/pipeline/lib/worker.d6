# The supervised worker that drives one pipeline stage. Everything here is a
# plain top-level def that captures nothing, which Supervisor#add_child
# requires: each attempt runs it in a fresh VM with a fresh heap.
#
# Each item travels as a "job": {"id", "item", "value", "attempt"}. `value`
# is what the next stage should process; `attempt` counts tries at THIS stage.

# Builds the command that runs one stage on one job. The stage is untrusted
# Diamond code, so it always runs under --sandbox in its OWN process, with
# the capabilities and budgets the host's manifest grants it -- the stage
# never chooses its own trust level.
def build_argv(diamond_bin, spec, job)
  env_args = ["DIAMOND_MAX_INSTRUCTIONS=#{spec["max_instructions"]}",
              "DIAMOND_MAX_WALL_MILLISECONDS=#{spec["max_wall_ms"]}"]
  allow = spec["allow"]
  if !allow.empty?()
    env_args = env_args + ["DIAMOND_SANDBOX_ALLOW=#{allow.join(",")}"]
  end
  ["env"] + env_args + [diamond_bin, "--sandbox", spec["path"], job["value"], job["attempt"].to_s()]
end

# Classifies a finished child from its exit status and stderr, the evidence
# an operator has. Returns [status, text]: "ok" with the stage's output, or
# "denied" / "budget" / "crashed" with the first line of its error.
def classify(result)
  return ["ok", result.stdout().strip()] if result.success?()
  first_line = result.stderr().strip().split("\n")[0]
  return ["denied", first_line] if result.stderr().include?("sandbox denies")
  return ["budget", first_line] if result.stderr().include?("resource limit exceeded")
  ["crashed", first_line]
end

# A job that is given up on, with the reason, goes to the dead-letter channel.
def dead_letter(dead, spec, job, reason, detail)
  dead.send({"id": job["id"], "stage": spec["name"], "item": job["item"],
             "reason": reason, "detail": detail, "attempt": job["attempt"]})
end

# The stage worker. It reads jobs from `input`, runs each through its
# sandboxed stage, and forwards results to `output`.
#
# Outcomes: a sandbox denial or exhausted budget is a policy decision, so the
# job is dead-lettered at once and the worker carries on. A crash is
# treated as transient: the worker records the job in `inflight`, then
# raises, so the SUPERVISOR restarts it. The restarted worker finds the job
# still in `inflight` and retries it with attempt + 1, until `max_attempts`.
# That is the whole point of the channel: a restart wipes the worker's heap, so
# the only memory that survives a crash is what lives in a Channel.
def stage_worker(diamond_bin, spec, max_attempts, input, output, dead, inflight)
  # Crash recovery: a job left in `inflight` was being processed when the
  # previous attempt died. try_receive raises WouldBlockError when it is empty,
  # which is the normal first-start case.
  job = nil
  begin
    job = inflight.try_receive()
    job["attempt"] = job["attempt"] + 1
  rescue error: WouldBlockError
    job = nil
  end

  loop do
    # Take the next job from upstream unless we are retrying one.
    if job == nil
      incoming = input.receive()
      break if incoming == nil
      job = {"id": incoming["id"], "item": incoming["item"],
             "value": incoming["value"], "attempt": 1}
    end

    # Park the job in `inflight` while it runs. A crash leaves it there.
    inflight.send(job)
    status, text = classify(Process.run(build_argv(diamond_bin, spec, job)))

    case status
    when "ok"
      inflight.receive()
      output.send({"id": job["id"], "item": job["item"], "value": text})
      job = nil
    when "crashed"
      if job["attempt"] >= max_attempts
        inflight.receive()
        dead_letter(dead, spec, job, "gave up", text)
        job = nil
      else
        # Transient until proven otherwise: crash this attempt on purpose
        # and let the supervisor restart us.
        raise "#{spec["name"]} crashed on #{job["item"]} (attempt #{job["attempt"]}): #{text}"
      end
    else
      inflight.receive()
      dead_letter(dead, spec, job, status, text)
      job = nil
    end
  end

  # No more input: pass the end-of-stream signal downstream.
  output.close()
end
