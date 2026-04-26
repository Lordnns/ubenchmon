# ubenchmon

**Benchmark Monitor Library + TUI** — A low-latency, minimally-intrusive tool
for setting up and monitoring latency-sensitive protocol benchmarks on Linux.

Built for the paper: *"Benchmarking Latency-Sensitive Network Protocols:
A Reproducible Methodology for Sub-Millisecond Measurements on Linux"*

## Architecture

```
┌────────────────────────────────────────────────────────┐
│                   ubenchmon TUI (Rust)                 │
│  ┌───────┐ ┌──────┐ ┌────────┐ ┌─────┐  ┌──────────┐   │
│  │Dashbrd│ │Setup │ │Monitor │ │Logs │  │Terminal  │   │
│  └───┬───┘ └──┬───┘ └───┬────┘ └──┬──┘  └────┬─────┘   │
│      └────────┴─────────┴─────────┴──────────┘         │
│                         │ FFI (bindgen)                │
├─────────────────────────┼──────────────────────────────┤
│              libubenchmon.so (C)                       │
│  ┌────────┐ ┌─────────┐ ┌────────┐ ┌────────────────┐  │
│  │ setup  │ │ monitor │ │ verify │ │ net passthru   │  │
│  │ .c     │ │ .c      │ │ .c     │ │ (kmod stub)    │  │
│  └────────┘ └─────────┘ └────────┘ └────────────────┘  │
├────────────────────────────────────────────────────────┤
│  Linux Kernel                                          │
│  ┌──────────┐ ┌───────────┐ ┌────────┐ ┌────────────┐  │
│  │perf_event│ │ sysfs/    │ │ netns  │ │ ubenchmon  │  │
│  │  (CPU)   │ │ procfs    │ │ + veth │ │ _net kmod  │  │
│  └──────────┘ └───────────┘ └────────┘ └────────────┘  │
└────────────────────────────────────────────────────────┘
```

## Two-Phase Design

### Phase 1: Setup (may require reboot)

The library configures the system according to the paper's methodology:

| Step                    | What it does                                              | Reboot? |
|------------------------|-----------------------------------------------------------|---------|
| GRUB / kernel params   | `isolcpus`, `nohz_full`, `rcu_nocbs`                      | **Yes** |
| SMT disable            | Runtime disable or BIOS recommendation                    | Maybe   |
| Frequency locking      | Disable boost, governor → performance                     | No      |
| IRQ affinity           | Migrate all IRQs to housekeeping core                     | No      |
| Service shutdown       | Stop irqbalance, timesyncd, etc.                          | No      |
| Swap disable           | `swapoff -a`                                              | No      |
| Network namespaces     | Create ns_server / ns_client + veth-srv / veth-cli pair   | No      |
| Offloading disable     | TSO / GSO / GRO off on veth interfaces                    | No      |
| NetEm                  | Apply delay / jitter / loss on veth (symmetrically)       | No      |
| Process isolation      | `taskset -c` core pinning + `chrt -f` RT scheduling       | No      |
| Sysctl tuning          | ASLR off, net buffer sizes, drop page cache               | No      |

When kernel parameters are modified, the library returns `UBENCHMON_ERR_REBOOT`
and the TUI shows a clear **"REBOOT REQUIRED"** modal.

The active configuration is persisted to `/var/lib/ubenchmon/active_config.json`
after every successful Apply so the daemon and teardown picker can reload it.
A timestamped config snapshot (`preconfig.json` + `config.json`) is saved to
`/var/lib/ubenchmon/snapshots/<ISO>/` before every Apply, giving a full
restore history accessible from the Teardown picker in the Setup tab.

### Phase 2: Monitor (zero-alloc hot path)

Once the system is configured, the monitor samples CPU, memory, network,
and disk at ~1-5 μs per snapshot:

- **CPU**: perf_event hardware counters (instructions, cycles, L3 cache misses),
  frequency via sysfs — all file descriptors pre-opened at init
- **Memory**: `sysinfo()` syscall (~200 ns)
- **Network**: sysfs counters via `pread()` on pre-opened fds
  (or kernel passthrough module for packet-level timestamping)
- **Disk**: `/sys/block/*/stat` via `pread()`

No `malloc`, no `open`/`close`, no blocking I/O on the hot path.

Every snapshot is logged as NDJSON to `/var/log/ubenchmon/metrics_<ISO>.jsonl`
(one JSON object per line, one line per subsystem per tick).

## TUI Tabs

| Tab       | Key | Purpose                                                 |
|-----------|-----|---------------------------------------------------------|
| Dashboard | F1  | System overview, verification checklist, live stats     |
| Setup     | F2  | Interactive config form, Apply / Teardown / snapshot picker |
| Monitor   | F3  | Live sparkline charts (CPU / MEM / NET / DISK)          |
| Logs      | F4  | Timestamped setup and runtime event log                 |
| Terminal  | F5  | Embedded shell — run any Linux command                  |

### Terminal passthrough

The Terminal tab lets you run arbitrary Linux commands without leaving
ubenchmon. Type `verify` for a quick system check, or run
`tc qdisc show`, `lscpu -e`, `ip netns exec ns_server ping 10.0.0.2`, etc.
Command history is available with ↑/↓ while the input bar is focused.

## Service / Daemon Mode

ubenchmon can run as a headless background daemon — useful when you want
continuous monitoring and NDJSON logging without keeping the TUI open, or
when the TUI should attach to an already-running monitor without interfering
with it ("piggyback mode").

```
                ┌───────────────────────────┐
                │     ubenchmon daemon       │
                │  monitor loop  100 ms tick │
                │  NDJSON logger             │
                │  writes snap.bin atomically│
                └───────────┬───────────────┘
                            │ /var/run/ubenchmon/snap.bin
                ┌───────────▼───────────────┐
                │   ubenchmon TUI (optional) │
                │   reads snap, piggyback    │
                └───────────────────────────┘
```

Two persistence modes are supported:

| Mode        | How to start              | Survives reboot? |
|-------------|---------------------------|------------------|
| Transient   | `ubenchmon service start` | No               |
| Persistent  | `ubenchmon service enable`| Yes (systemd)    |

In piggyback mode the status bar shows `◉ SVC[T]` (transient) or
`◉ SVC[P]` (persistent). Quitting the TUI does **not** stop the daemon.

### Service subcommands

```bash
ubenchmon service start    # Start transient background daemon (no reboot persistence)
ubenchmon service stop     # Send SIGTERM to daemon, clean up snap.bin
ubenchmon service enable   # Install systemd unit + start (restarts on boot)
ubenchmon service disable  # Stop, disable, and remove the systemd unit
ubenchmon service status   # Show PID, mode, last snapshot age, log file path
```

The systemd unit is written to `/etc/systemd/system/ubenchmon.service` and
re-applies the runtime setup on every boot (network namespaces, swap, IRQ
affinity, and frequency governor do not survive a reboot).

## Build

### Prerequisites

```bash
# Debian / Ubuntu
sudo apt install build-essential libclang-dev ethtool iproute2

# Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

> **No libclang?** Pre-generated bindings are included at
> `tui/src/bindings_pregenerated.rs` as a fallback.
> The build picks them up automatically if bindgen cannot run.

### C library only

```bash
cd ubenchmon
make
# → libubenchmon.so, libubenchmon.a
```

### Full TUI (recommended)

```bash
cd ubenchmon/tui
cargo build --release
# → target/release/ubenchmon
```

### Install

**Option 1: Pre-built package (recommended)**

```bash
# Check the Releases page for the current version tag
wget https://github.com/Lordnns/benchmon/releases/download/{VERSION}/ubenchmon.deb
sudo dpkg -i ubenchmon.deb
```

This installs the `ubenchmon` TUI binary, `libubenchmon.so`/`.a`, and
`ubenchmon.h` in the standard system paths.

**Option 2: Build from source**

```bash
# 1. Library + headers
sudo make install   # → /usr/local/lib + /usr/local/include

# 2. TUI binary
sudo cp tui/target/release/ubenchmon /usr/local/bin/
```

### Developing with libubenchmon

```c
#include <ubenchmon.h>
```

```bash
gcc -o my_app my_app.c -lubenchmon
```

## Usage

```bash
# Interactive TUI — full functionality requires root
sudo ubenchmon

# Non-root: TUI opens with a warning modal; monitor and verify still work
ubenchmon

# Service management (requires root for enable/disable)
sudo ubenchmon service start
sudo ubenchmon service status
sudo ubenchmon service stop

# Emit the taskset/chrt launch prefix for benchmark scripts
PREFIX=$(ubenchmon launch-prefix server)
sudo ip netns exec ns_server $PREFIX ./my_server --port 8000

PREFIX=$(ubenchmon launch-prefix client)
sudo ip netns exec ns_client $PREFIX ./my_client --host 10.0.0.1
```

`launch-prefix` reads the active config from
`/var/lib/ubenchmon/active_config.json` and emits a string such as
`taskset -c 2,4 chrt -f 50 ` that can be prepended to any benchmark
process invocation.

## Runtime Paths

| Path                                        | Purpose                                     |
|---------------------------------------------|---------------------------------------------|
| `/var/lib/ubenchmon/active_config.json`     | Last-applied setup config (JSON)            |
| `/var/lib/ubenchmon/snapshots/<ISO>/`       | Config snapshots before every Apply         |
| `/var/run/ubenchmon/snap.bin`               | Latest monitor snapshot (daemon IPC)        |
| `/var/run/ubenchmon/benchmon.pid`           | Daemon PID file                             |
| `/var/run/ubenchmon/service.mode`           | `transient` or `persistent`                 |
| `/var/log/ubenchmon/metrics_<ISO>.jsonl`    | NDJSON metric log (one object per line)     |
| `/etc/systemd/system/ubenchmon.service`     | Systemd unit (persistent mode only)         |

### NDJSON log format

```jsonl
{"type":"latency","time":"2026-04-01T06:34:17.123456","capture_ns":3210}
{"type":"cpu","time":"...","core":2,"freq_mhz":3600,"cycles":...,"instructions":...,"cache_misses":...,"usage_pct":1.2345}
{"type":"mem","time":"...","total_bytes":...,"used_bytes":...,"available_bytes":...,"cache_bytes":...,"swap_used_bytes":0}
{"type":"net","time":"...","iface":"veth-srv","rx_bytes":...,"tx_bytes":...,...}
{"type":"disk","time":"...","dev":"nvme0n1","reads":...,"writes":...,...}
{"type":"event","time":"...","event":"setup","detail":"Applied: namespaces, netem, swap off"}
```

## Kernel Passthrough Module (Future)

The network monitoring includes a stub for the kernel passthrough module
(`ubenchmon_net_kmod`). When loaded, it:

1. Hooks into the NIC driver's packet path
2. Stamps each packet with hardware + software timestamps
3. Writes to a lock-free ring buffer (shared memory)
4. Userspace reads via `mmap()` — **zero syscalls on the read path**

The API is already defined in `ubenchmon.h`:

```c
ubenchmon_net_passthru_available()           // check if module is loaded
ubenchmon_net_passthru_open(iface)           // open ring buffer → fd
ubenchmon_net_passthru_read(fd, events, max) // non-blocking batch read
ubenchmon_net_passthru_close(fd)             // cleanup
```

## API (C)

```c
#include <ubenchmon.h>

// Phase 1: Setup
ubenchmon_setup_config_t cfg = {
    .isolated_cores       = (int[]){2,3,4,5},
    .isolated_cores_count = 4,
    .housekeeping_core    = 0,
    .disable_frequency_boost = 1,
    .max_cstate           = 0,
    .stop_irqbalance      = 1,
    .disable_swap         = 1,
    .ns_server_name       = "ns_server",
    .ns_client_name       = "ns_client",
    .veth_server_name     = "veth-srv",
    .veth_client_name     = "veth-cli",
    .server_ip            = "10.0.0.1/24",
    .client_ip            = "10.0.0.2/24",
    .netem_delay_ms       = 25,
    .netem_jitter_ms      = 5,
    .netem_loss_pct       = 0.1,
    .disable_offloading   = 1,
    .server_cores         = "2,4",
    .client_cores         = "3,5",
    .rt_priority          = 50,
    .disable_aslr         = 1,
    .tune_net_buffers     = 1,
    .drop_caches          = 1,
    .stop_timesyncd       = 1,
    .apply_nohz_full      = 1,
    .apply_rcu_nocbs      = 1,
};

ubenchmon_setup_result_t result;
ubenchmon_status_t rc = ubenchmon_setup(&cfg, &result);
if (rc == UBENCHMON_ERR_REBOOT) {
    printf("Reboot required: %s\n", result.message);
}

// Get launch prefix for benchmark scripts
char *prefix = ubenchmon_get_launch_prefix(&cfg, /*is_server=*/1);
// → "taskset -c 2,4 chrt -f 50 "
free(prefix);

// Phase 2: Monitor
ubenchmon_monitor_t *mon = ubenchmon_monitor_init(UBENCHMON_MON_ALL, NULL, 0);
ubenchmon_snapshot_t snap;
ubenchmon_snapshot(mon, &snap);  // ~1-5 μs, zero heap allocation
printf("Capture latency: %lu ns\n", snap.capture_latency_ns);

// Cleanup
ubenchmon_monitor_destroy(mon);
ubenchmon_teardown(&cfg);
```

## License

MIT