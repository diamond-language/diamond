require 'json'
pid=Integer(ARGV.fetch(0)); output=ARGV.fetch(1); phase_file=ARGV.fetch(2)
database=ARGV.fetch(3); cap=Integer(ENV.fetch('SOAK_RSS_CAP_KIB','2000000'))
def identity(pid)
  File.read("/proc/#{pid}/stat").rpartition(') ').last.split[19]
end
started=Process.clock_gettime(Process::CLOCK_MONOTONIC)
begin
  original=identity(pid)
  File.open(output,'w') do |file|
    file.sync=true
    loop do
      break unless identity(pid)==original
      status=File.read("/proc/#{pid}/status")
      break if status.match?(/^State:\s+Z/)
      smaps=begin File.read("/proc/#{pid}/smaps_rollup");rescue Errno::EACCES;'';end
      value=->(text,name) { match=text.match(/^#{Regexp.escape(name)}:\s+(\d+)/);match&&match[1].to_i }
      size=->(path) { File.size?(path)||0 }
      phase=File.exist?(phase_file) ? File.read(phase_file).strip : 'startup'
      row={pid:pid,seconds:Process.clock_gettime(Process::CLOCK_MONOTONIC)-started,
        phase:phase,rss_kib:value.call(status,'VmRSS'),hwm_kib:value.call(status,'VmHWM'),
        pss_kib:value.call(smaps,'Pss'),private_dirty_kib:value.call(smaps,'Private_Dirty'),
        swap_kib:value.call(status,'VmSwap'),threads:value.call(status,'Threads'),
        db_bytes:size.call(database),wal_bytes:size.call(database+'-wal')}
      file.puts(JSON.generate(row))
      if row[:rss_kib]&&row[:rss_kib]>cap
        File.write(output+'.cap-tripped',JSON.generate(row))
        Process.kill('KILL',pid)
        break
      end
      sleep Float(ENV.fetch('SOAK_SAMPLE_SECONDS','1'))
    end
  end
rescue Errno::ENOENT,Errno::ESRCH
  # A normally exited/reaped process no longer has a /proc entry.
end
