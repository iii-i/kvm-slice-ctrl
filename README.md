# Spinlock contention measurement

- Build vmlinux, perf, and QEMU
- Bundle virtme-ng (distros ship an old one)
- Use distro dbench
- Start a single VM (vCPUs:pCPUs = 2:1)
- Start dbench (clients:vCPUs = 1:1)
