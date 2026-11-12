require 'json'
workspace=ARGV.fetch(0)
def median(values)
  values=values.compact.sort
  return nil if values.empty?
  (values[(values.length-1)/2]+values[values.length/2])/2.0
end
def slope(rows,key)
  return nil if rows.length<2
  x=rows.map { |r| r.fetch('seconds') };y=rows.map { |r| r[key] }
  return nil if y.any?(&:nil?)
  mx=x.sum/x.length;my=y.sum/y.length
  denominator=x.sum { |n| (n-mx)**2 }
  denominator>0 ? x.zip(y).sum { |a,b| (a-mx)*(b-my) }/denominator*60/1024 : 0
end
summary=[]
%w[pheint SkindicateV3].each do |app|
  %w[rc3 candidate].each do |variant|
    [1,4].each do |workers|
      directory=File.join(workspace,'results',"#{app}-#{variant}-w#{workers}")
      files=Dir.glob(directory+'/server-*.memory.jsonl')
      abort("expected one serving process: #{directory}") unless files.length==1
      rows=File.readlines(files.first).map { |line| JSON.parse(line) }
      abort("watchdog tripped: #{directory}") unless Dir.glob(directory+'/*.cap-tripped').empty?
      abort("swap observed: #{directory}") if rows.any? { |r| r['swap_kib'].to_i>0 }
      phases=rows.group_by { |r| r['phase'] }
      loaded=rows.select { |r| r['phase'].start_with?('load-') }
      quiet=phases.fetch('final-quiet',[])
      abort("missing sustained load/quiet samples: #{directory}") if loaded.length<150||quiet.length<15
      late=loaded.drop(loaded.length/2)
      before_last=phases.fetch('quiet-3',[])
      initial=median(before_last.map { |r| r['rss_kib'] })
      final=median(quiet.map { |r| r['rss_kib'] })
      delta=(final-initial)/1024
      gc=File.readlines(Dir.glob(directory+'/server-*.log').fetch(0)).filter_map do |line|
        match=line.match(/SOAK_GC kind=(\w+) seconds=([\d.]+) thread=(\d+) major=(\d+) minor=(\d+) jit=(\d+) live_bytes=(\d+)/)
        match&&{kind:match[1],seconds:match[2].to_f,thread:match[3],major:match[4].to_i,minor:match[5].to_i,jit:match[6].to_i,live_bytes:match[7].to_i}
      end
      abort("no real GC/JIT evidence: #{directory}") if gc.empty?||gc.none? { |r| r[:jit]>0 }
      work=JSON.parse(File.read(directory+'/workload.json'))
      if app=='pheint'
        abort('Pheint semantic checks/errors') unless work['verified_checks']>=47&&work['scenarios'].all? { |r| r['error_count']==0 }
        count=work.fetch('round_samples').values.flatten.sum { |r| r.fetch('count') }
        rps=work.fetch('scenarios').fetch(0).fetch('rps')
      else
        abort('Skindicate semantic coverage') unless work['verified_actors']==8
        count=work.fetch('rounds').sum { |r| r.fetch('requests') }
        rps=work.fetch('rounds').sum { |r| r.fetch('rps') }/work['rounds'].length
      end
      row={app:app,variant:variant,workers:workers,requests:count,rps:rps,
        peak_rss_mib:rows.map { |r| r['rss_kib'] }.compact.max/1024.0,
        final_quiet_rss_mib:final/1024,late_load_rss_mib:median(late.map { |r| r['rss_kib'] })/1024,
        late_load_pss_mib:median(late.map { |r| r['pss_kib'] })&./(1024),
        final_round_retained_mib:delta,late_private_slope_mib_per_min:slope(late,'private_dirty_kib'),
        gc_checkpoints:gc.length,gc_threads:gc.map { |r| r[:thread] }.uniq.length,
        max_major:gc.map { |r| r[:major] }.max,max_minor:gc.map { |r| r[:minor] }.max,
        max_jit:gc.map { |r| r[:jit] }.max,db_warm:JSON.parse(File.read(directory+'/db-warm.json')),
        db_final:JSON.parse(File.read(directory+'/db-final.json'))}
      summary<<row
      puts "#{app.ljust(13)} #{variant.ljust(9)} w#{workers}: #{row[:late_load_rss_mib].round(1)} MiB loaded, #{row[:final_quiet_rss_mib].round(1)} MiB quiet, retained #{delta.round(2)} MiB, #{count} checked requests"
    end
  end
end
File.write(File.join(workspace,'summary.json'),JSON.pretty_generate(summary)+"\n")
suspect=summary.select { |r| r[:variant]=='candidate'&&(r[:final_round_retained_mib]>8||r[:late_private_slope_mib_per_min].to_f>4) }
unless suspect.empty?
  warn "Memory growth needs review: #{suspect.map { |r| [r[:app],r[:workers]] }.inspect}"
  exit 2
end
