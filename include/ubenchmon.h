//
// ubenchmon.h — μs-Benchmark Monitor Library
//
// A low-latency, minimally-intrusive shared library for setting up
// and monitoring latency-sensitive protocol benchmarks on Linux.
// Designed for reproducible sub-millisecond (μs-level) measurements.
//
// Two-phase design:
//   Phase 1 (Setup):  Configure kernel params, CPU isolation, network
//                      namespaces, IRQ affinity. May require reboot.
//   Phase 2 (Monitor): Ultra-low-overhead sampling of CPU, memory,
//                      network, and disk — designed for constant-time
//                      retrieval with no heap allocation on the hot path.
//
// SPDX-License-Identifier: MIT
//

#ifndef UBENCHMON_H
#define UBENCHMON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

//  Version
#define UBENCHMON_VERSION_MAJOR 0
#define UBENCHMON_VERSION_MINOR 1
#define UBENCHMON_VERSION_PATCH 0

//  Return codes
typedef enum {
  UBENCHMON_OK = 0,
  UBENCHMON_ERR_PERM = -1,    // Need root / CAP_SYS_ADMIN
  UBENCHMON_ERR_REBOOT = -2,  // Changes applied, reboot needed
  UBENCHMON_ERR_PARTIAL = -3, // Some settings applied, some not
  UBENCHMON_ERR_IO = -4,      // File / sysfs I/O failure
  UBENCHMON_ERR_INVAL = -5,   // Invalid argument
  UBENCHMON_ERR_NOTSUP = -6,  // Feature not available on hw
  UBENCHMON_ERR_ALREADY = -7, // Already configured
  UBENCHMON_ERR_BUSY = -8,    // Resource in use
} ubenchmon_status_t;

// ------------------------------------------------------------------
//  Setup — Phase 1
// ------------------------------------------------------------------

// Describes the desired benchmark environment.
// Fill in what you want; leave fields at 0/NULL for "don't touch".
typedef struct {
  // CPU isolation --------------------------------------------------
  const int *isolated_cores; // Array of core IDs to isolate
  int isolated_cores_count;
  int housekeeping_core; // Core for OS/IRQs (default 0)

  // CPU frequency --------------------------------------------------
  int disable_frequency_boost; // 1 = disable turbo/CPB
  int lock_frequency_mhz;      // 0 = don't touch

  // C-states -------------------------------------------------------
  int max_cstate; // 0 = C0 only (lowest latency)

  // Services -------------------------------------------------------
  int stop_irqbalance;   // 1 = stop irqbalance service
  int disable_swap;      // 1 = swapoff -a
  int isolate_multiuser; // 1 = systemctl isolate multi-user

  // Network namespaces ---------------------------------------------
  const char *ns_server_name;   // e.g. "ns_server"
  const char *ns_client_name;   // e.g. "ns_client"
  const char *veth_server_name; // e.g. "veth-s"
  const char *veth_client_name; // e.g. "veth-c"
  const char *server_ip;        // e.g. "10.0.0.1/24"
  const char *client_ip;        // e.g. "10.0.0.2/24"

  // NetEm impairment (applied symmetrically) -----------------------
  int netem_delay_ms;    // One-way delay in ms
  int netem_jitter_ms;   // Jitter in ms (normal dist)
  double netem_loss_pct; // Loss % per direction

  // Offloading -----------------------------------------------------
  int disable_offloading; // 1 = disable TSO/GSO/GRO

  // Process isolation (stored here so scripts need zero hardcoding)
  const char *server_cores;  // taskset -c arg, e.g. "2,4"
  const char *client_cores;  // taskset -c arg, e.g. "3,5"
  int         rt_priority;   // chrt -f value; 0 = no RT sched

  // Sysctl tuning --------------------------------------------------
  int disable_aslr;       // 1 = randomize_va_space → 0
  int tune_net_buffers;   // 1 = apply rmem/wmem tuning
  int drop_caches;        // 1 = echo 3 > drop_caches
  int stop_timesyncd;     // 1 = stop systemd-timesyncd

  // GRUB kernel params — applied to isolated_cores list ------------
  int apply_nohz_full;    // 1 = add nohz_full=<cores>
  int apply_rcu_nocbs;    // 1 = add rcu_nocbs=<cores>

} ubenchmon_setup_config_t;

// Result of a setup operation.
typedef struct {
  ubenchmon_status_t status;
  int reboot_required; // 1 if reboot needed
  char message[512];   // Human-readable summary

  // What was actually applied
  int grub_modified;
  int irq_migrated;
  int namespaces_created;
  int netem_applied;
  int offloading_disabled;
  int swap_disabled;
  int services_stopped;
  int frequency_locked;
  int sysctl_tuned;
  int caches_dropped;
  int process_isolation_ready;
} ubenchmon_setup_result_t;

// Apply the full setup configuration.
// Must be called as root.  May set reboot_required=1.
//
// Returns UBENCHMON_OK if everything applied (or already set).
// Returns UBENCHMON_ERR_REBOOT if kernel boot params were modified.
// Returns UBENCHMON_ERR_PARTIAL if some items failed (check result).
ubenchmon_status_t ubenchmon_setup(const ubenchmon_setup_config_t *cfg,
                                 ubenchmon_setup_result_t *result);

// Full teardown: reverts the system to its exact pre-ubenchmon state.
// A reboot is required after teardown if GRUB was modified.
ubenchmon_status_t ubenchmon_teardown(const ubenchmon_setup_config_t *cfg);

// Return a heap-allocated shell prefix for launching a benchmark process.
// is_server: 1 = use server_cores, 0 = use client_cores.
// Example: "taskset -c 2,4 chrt -f 50 "
// Caller must free() the returned string.
char *ubenchmon_get_launch_prefix(const ubenchmon_setup_config_t *cfg,
                                 int is_server);

// ------------------------------------------------------------------
//  Verify — Pre-flight checks
// ------------------------------------------------------------------

typedef struct {
  // CPU
  int smt_disabled;           // 1 = confirmed off
  int threads_per_core;       // Should be 1
  int cores_isolated;         // 1 = isolcpus active
  int isolated_core_list[64]; // Which cores are isolated
  int isolated_core_count;
  int nohz_full_active;    // 1 = tickless on isolated cores
  int frequency_boost_off; // 1 = turbo/CPB disabled
  int actual_freq_mhz;     // Measured frequency

  // Memory
  int swap_off; // 1 = no swap active
  uint64_t total_ram_mb;
  uint64_t available_ram_mb;

  // Network
  int ns_server_exists; // 1 = namespace exists
  int ns_client_exists;
  int veth_link_up;        // 1 = veth pair is up
  int offloading_disabled; // 1 = TSO/GSO/GRO off
  int netem_active;        // 1 = netem qdisc attached

  // System
  int irqbalance_stopped;
  int running_bare_metal; // 1 = not a VM
  char kernel_version[64];
  char cpu_model[128];
  char hypervisor[64]; // Empty if bare-metal

  // Overall
  int all_checks_passed; // 1 = everything looks good
  char warnings[1024];   // Newline-separated warnings
} ubenchmon_verify_result_t;

// Run all pre-flight verification checks.
// Does not modify the system. Safe to call without root.
ubenchmon_status_t ubenchmon_verify(ubenchmon_verify_result_t *result, const ubenchmon_setup_config_t *cfg);

// ------------------------------------------------------------------
//  Monitor — Phase 2  (hot path, zero-alloc, constant-time)
// ------------------------------------------------------------------

// Opaque handle for the monitor subsystem.
// Created once, used many times on the hot path.
typedef struct ubenchmon_monitor ubenchmon_monitor_t;

// Which subsystems to monitor.  Bitfield.
typedef enum {
  UBENCHMON_MON_CPU = (1 << 0),
  UBENCHMON_MON_MEMORY = (1 << 1),
  UBENCHMON_MON_NETWORK = (1 << 2),
  UBENCHMON_MON_DISK = (1 << 3),
  UBENCHMON_MON_ALL = 0x0F,
} ubenchmon_mon_flags_t;

// CPU snapshot — per-core, read from perf_event / sysfs.
typedef struct {
  int core_id;
  uint64_t timestamp_ns; // CLOCK_MONOTONIC_RAW
  uint64_t tsc;          // rdtsc value
  uint64_t instructions; // HW perf counter
  uint64_t cycles;       // HW perf counter
  uint64_t cache_misses; // HW perf counter (L3)
  uint32_t freq_mhz;     // Current frequency
  uint32_t cstate;       // Current C-state (0 = C0)
  double usage_pct;      // CPU usage since last sample
} ubenchmon_cpu_sample_t;

// Memory snapshot — system-wide, from sysinfo() syscall.
typedef struct {
  uint64_t timestamp_ns;
  uint64_t total_bytes;
  uint64_t available_bytes;
  uint64_t used_bytes;
  uint64_t cache_bytes;
  uint64_t swap_used_bytes; // Should be 0 if setup correct
} ubenchmon_mem_sample_t;

// Network snapshot — per-interface, from /sys/class/net or passthru.
// NOTE: The kernel passthrough module (ubenchmon_net_kmod) replaces
//       this with direct packet-level capture when loaded.
typedef struct {
  uint64_t timestamp_ns;
  char iface[16]; // Interface name
  uint64_t rx_bytes;
  uint64_t tx_bytes;
  uint64_t rx_packets;
  uint64_t tx_packets;
  uint64_t rx_dropped;
  uint64_t tx_dropped;
  uint64_t rx_errors;
  uint64_t tx_errors;

  // Populated by kernel passthrough module only
  int passthru_active; // 1 = kernel module providing data
} ubenchmon_net_sample_t;

// Packet-level capture from the kernel passthrough module.
// This is the ultra-low-latency path: the kernel module stamps
// each packet with a hardware timestamp and writes to a shared
// ring buffer — no syscall on the read path.
typedef struct {
  uint64_t hw_timestamp_ns; // NIC hardware timestamp
  uint64_t sw_timestamp_ns; // Kernel ktime_get_raw_ns()
  uint32_t packet_id;       // Application-level packet ID
  uint32_t packet_size;     // Bytes (L3 payload)
  uint8_t direction;        // 0 = RX, 1 = TX
  uint8_t protocol;         // IPPROTO_TCP, _UDP, etc.
} ubenchmon_packet_event_t;

// Disk snapshot — per-device, from /sys/block/(dev)/stat.
typedef struct {
  uint64_t timestamp_ns;
  char device[32]; // e.g. "nvme0n1"
  uint64_t reads_completed;
  uint64_t writes_completed;
  uint64_t read_bytes;
  uint64_t write_bytes;
  uint64_t io_in_progress;
  uint64_t io_time_ms; // Total ms spent doing I/O
} ubenchmon_disk_sample_t;

// Combined snapshot — everything in one struct, one call.
typedef struct {
  uint64_t timestamp_ns;       // When snapshot was taken
  uint64_t capture_latency_ns; // How long the capture took

  ubenchmon_cpu_sample_t cpu[64]; // Up to 64 cores
  int cpu_count;

  ubenchmon_mem_sample_t mem;

  ubenchmon_net_sample_t net[8]; // Up to 8 interfaces
  int net_count;

  ubenchmon_disk_sample_t disk[8]; // Up to 8 devices
  int disk_count;
} ubenchmon_snapshot_t;

// Initialize the monitor subsystem.
// Opens perf_event file descriptors, maps sysfs files, etc.
// Call once before the hot path.
//
// @param flags       Which subsystems to monitor (UBENCHMON_MON_*).
// @param cores       Array of core IDs to monitor (NULL = all).
// @param core_count  Number of cores in the array.
// @return            Opaque handle, or NULL on failure.
ubenchmon_monitor_t *ubenchmon_monitor_init(ubenchmon_mon_flags_t flags,
                                          const int *cores, int core_count);

// Take a snapshot.  HOT PATH — no heap allocation, no blocking I/O.
// Designed for constant-time execution (~1-5 μs depending on flags).
//
// @param mon   Handle from ubenchmon_monitor_init().
// @param snap  Pre-allocated snapshot struct (caller owns).
// @return      UBENCHMON_OK on success.
ubenchmon_status_t ubenchmon_snapshot(ubenchmon_monitor_t *mon,
                                    ubenchmon_snapshot_t *snap);

// Destroy the monitor, close file descriptors, unmap memory.
void ubenchmon_monitor_destroy(ubenchmon_monitor_t *mon);

// ------------------------------------------------------------------
//  Kernel Passthrough — Network Module Interface
// ------------------------------------------------------------------

// Check if the kernel passthrough module is loaded.
int ubenchmon_net_passthru_available(void);

// Open the shared ring buffer from the kernel module.
// Returns a file descriptor to mmap(), or -1 on failure.
// The ring buffer is lock-free: the kernel writes, userspace reads.
int ubenchmon_net_passthru_open(const char *iface);

// Read the next batch of packet events from the ring buffer.
// Non-blocking.  Returns the number of events read (0 = none ready).
//
// @param fd      File descriptor from ubenchmon_net_passthru_open().
// @param events  Caller-allocated array.
// @param max     Maximum events to read.
// @return        Number of events read, or -1 on error.
int ubenchmon_net_passthru_read(int fd, ubenchmon_packet_event_t *events,
                               int max);

// Close the passthrough ring buffer.
void ubenchmon_net_passthru_close(int fd);

// ------------------------------------------------------------------
//  Utility
// ------------------------------------------------------------------

// High-resolution timestamp (CLOCK_MONOTONIC_RAW), no syscall
// overhead on kernels with vDSO support.
static inline uint64_t ubenchmon_now_ns(void);

// Read the TSC (Time Stamp Counter) directly.
// Useful for sub-nanosecond interval measurement.
static inline uint64_t ubenchmon_rdtsc(void);

// Get the last error message (thread-local).
const char *ubenchmon_strerror(void);

// Print a full system report to the given file descriptor.
// Useful for embedding in benchmark output.
ubenchmon_status_t ubenchmon_report(int fd);

#ifdef __cplusplus
}
#endif

// ------------------------------------------------------------------
//  Inline implementations
// ------------------------------------------------------------------

#include <time.h>

static inline uint64_t ubenchmon_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#if defined(__x86_64__) || defined(__i386__)
static inline uint64_t ubenchmon_rdtsc(void) {
  uint32_t lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
#elif defined(__aarch64__)
static inline uint64_t ubenchmon_rdtsc(void) {
  uint64_t val;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
  return val;
}
#else
static inline uint64_t ubenchmon_rdtsc(void) {
  return ubenchmon_now_ns(); // Fallback
}
#endif

#endif
