require 'tmpdir'
require 'json'
require 'rbconfig'
sampler=File.join(__dir__,'sample.rb')
Dir.mktmpdir('soak-sampler-') do |work|
  phase=File.join(work,'phase');File.write(phase,'warmup')
  child=Process.spawn('sleep','30')
  monitor=nil
  begin
    output=File.join(work,'normal.jsonl')
    monitor=Process.spawn({'SOAK_SAMPLE_SECONDS'=>'0.02'},RbConfig.ruby,sampler,child.to_s,output,phase,work+'/db')
    sleep 0.3
    File.write(phase,'quiet')
    sleep 0.3
    Process.kill('TERM',child);Process.wait(child);child=nil
    Process.wait(monitor);raise 'normal monitor failed' unless $?.success?;monitor=nil
    rows=File.readlines(output).map { |line| JSON.parse(line) }
    raise 'missing phase transition' unless %w[warmup quiet].all? { |name| rows.any? { |r| r['phase']==name } }
    raise 'missing memory data' unless rows.all? { |r| r['rss_kib']&&r['rss_kib']>0 }
    raise 'timestamps went backward' unless rows.each_cons(2).all? { |a,b| b['seconds']>=a['seconds'] }
    child=Process.spawn('sleep','30')
    output=File.join(work,'cap.jsonl')
    monitor=Process.spawn({'SOAK_RSS_CAP_KIB'=>'1','SOAK_SAMPLE_SECONDS'=>'0.02'},RbConfig.ruby,sampler,child.to_s,output,phase,work+'/db')
    Process.wait(child);raise 'watchdog did not kill its target' unless $?.signaled?&&$?.termsig==9;child=nil
    Process.wait(monitor);raise 'watchdog failed' unless $?.success?;monitor=nil
    raise 'watchdog evidence missing' unless File.exist?(output+'.cap-tripped')
    puts 'sampler lifecycle, phase, monotonic-clock and memory-cap checks passed'
  ensure
    [child,monitor].compact.each do |pid|
      begin Process.kill('KILL',pid);Process.wait(pid);rescue Errno::ESRCH,Errno::ECHILD;end
    end
  end
end
