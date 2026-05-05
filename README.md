# Spinlock contention measurement

## Usage

```
./install-dependencies
./build
./run
```

## Output

- With `TRACER=trace-cmd`: `workdir/trace-{host,guest}.dat`
- With `TRACER=perfetto`: `workdir/perfetto.trace`

## Methodology

- Build vmlinux (`CONFIG_LOCK_STAT`, `CONFIG_PARAVIRT_SPINLOCKS`), perf, QEMU
- Bundle virtme-ng (distros ship an old one)
- Use distro dbench
- Single VM: 8 vCPUs pinned to 4 host pCPUs (2:1 overcommit, to force lock holder preemption), 16G RAM (600M * 8 CPUs * 2 ~= 10G for in-kernel per-CPU ftrace ring buffer, plus trace-cmd's `--temp /tmp/` spill, plus 4G dbench tmpfs, plus headroom)
- Workload: dbench on tmpfs (~80% guest CPU in spinlocks, stresses KVM lock-wait paths), 1 client per vCPU, 4s (to make sure all traces fit into per-CPU ftrace ring buffers)
- `TRACER=perfetto` or `TRACER=trace-cmd` selects the tracer (unset = no trace)
- Host events: `kvm/{entry,exit,msr}`
- Guest events: `lock/{acquire,contended,acquired,release}`, `msr/write_msr`, `sched/sched_process_exec`
- ftrace clock: TAI; guest synchronized using `phc2sys` from `/dev/ptp0`
- Timestamp consistency validated by paired `wrmsr`/`kvm_msr` entries (~2us)
- Ultimate idea: correlate host `kvm_exit` with guest `lock_acquired` to attribute contention to off-guest time
