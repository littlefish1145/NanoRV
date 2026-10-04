"""Read counters and nested timing samples from a completed NanoRV UART log."""
import argparse,json,pathlib,re
p=argparse.ArgumentParser();p.add_argument('log');p.add_argument('--output');a=p.parse_args()
text=pathlib.Path(a.log).read_text(encoding='utf-8',errors='replace')
totals={}
per_hart={}
for line in text.splitlines():
    if any(tag in line for tag in ['JIT profile hart','JIT detail hart','JIT sample hart']):
        hart=int(re.search(r'hart ([0-9]+)',line)[1])
        fields=per_hart.setdefault(hart,{})
        for name,value in re.findall(r'(\w+)=([0-9]+)',line):totals[name]=totals.get(name,0)+int(value)
        fields.update((name,int(value)) for name,value in re.findall(r'(\w+)=([0-9]+)',line))
native=totals.get('native_insn',0);cold=totals.get('cold_insn',0)
sample_time=sum(totals.get(name,0) for name in ['lookup_ns','native_ns','cold_ns'])
report={'totals':totals,'interpretation':{
    'native_insn':'Guest instructions retired in JIT blocks, including instructions executed by C++ helpers.',
    'cold_insn':'Guest instructions retired through predecoded C++ executors: warmup, compiler wait or budget tail.',
    'self_loop':'Backedges completed inside a native block, not dispatcher returns.',
    'link_miss':'Fast successor lookup misses; slow_link_hit records recovery without dispatcher exit.',
    'timing':'One dispatcher invocation in 4096 per hart; host wall times, with instrumentation and scheduler noise.',
    'resolver_ns':'Nested subset of native_ns. Do not add it again to total sample time.',
    'scope':'Whole VM session including boot, commands and shutdown; not exclusively benchmark hot loops.'}}
if native+cold:report['jit_guest_coverage_percent']=100*native/(native+cold)
if sample_time:
    report['sample_time_percent']={name:100*totals.get(name,0)/sample_time for name in ['lookup_ns','native_ns','cold_ns','resolver_ns']}
if totals.get('sampled_native_insn'):report['sampled_helper_calls_per_jit_guest_instruction']=totals.get('sampled_helper_calls',0)/totals['sampled_native_insn']
if cold:report['cpp_path_breakdown_percent']={name:100*totals.get(name,0)/cold for name in ['tail_insn','pending_insn','warming_insn']}
if totals.get('link_miss'):report['slow_lookup_recovery_percent']=100*totals.get('slow_link_hit',0)/totals['link_miss']
if totals.get('lookup_ticks'):
    corrected={name:0 for name in ['lookup_ticks','native_ticks','cold_ticks','resolver_ticks']}
    for fields in per_hart.values():
        cost=fields.get('timer_call_ticks',0);resolvers=fields.get('resolver_samples',0)
        corrected['lookup_ticks']+=max(0,fields.get('lookup_ticks',0)-cost*fields.get('samples',0))
        corrected['cold_ticks']+=max(0,fields.get('cold_ticks',0)-cost*fields.get('cold_samples',0))
        corrected['resolver_ticks']+=max(0,fields.get('resolver_ticks',0)-cost*resolvers)
        corrected['native_ticks']+=max(0,fields.get('native_ticks',0)-cost*(fields.get('native_samples',0)+2*resolvers))
    measured=sum(corrected[name] for name in ['lookup_ticks','native_ticks','cold_ticks'])
    report['clock_cost_corrected_ticks']=corrected
    report['nested_timing_consistent']=corrected['resolver_ticks']<=corrected['native_ticks']
    if measured:report['clock_cost_corrected_percent']={name:100*value/measured for name,value in corrected.items()}
    report['interpretation']['timing']='1/4096 dispatcher calls; ordered TSC timestamps on x64. Host time includes scheduling. Percentages subtract calibrated timer-call cost, approximately.'
    report['interpretation']['resolver_ticks']='Nested subset of native_ticks; do not add twice. Calibration does not remove all profiler overhead.'
output=json.dumps(report,indent=2,ensure_ascii=False)
if a.output:pathlib.Path(a.output).write_text(output+'\n',encoding='utf-8')
print(output)
