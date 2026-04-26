//
// setup.c — Phase 1: System configuration for benchmarking
//
// Each sub-step is idempotent — calling setup twice is safe.
// Returns UBENCHMON_ERR_REBOOT when kernel boot params were changed.
//

#define _GNU_SOURCE
#include "ubenchmon_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>


//  Helpers
static int is_root(void) { return geteuid() == 0; }

static void result_append(ubenchmon_setup_result_t *r, const char *fmt, ...) {
    size_t len = strlen(r->message);
    if (len >= sizeof(r->message) - 2) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r->message + len, sizeof(r->message) - len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        len += (size_t)n;
        if (len < sizeof(r->message) - 2) {
            r->message[len] = '\n';
            r->message[len + 1] = '\0';
        }
    }
}


//  GRUB preconfig save / restore
//
//  Stores exactly what nohz_full= and rcu_nocbs= looked like in
//  /proc/cmdline BEFORE ubenchmon ever touches GRUB.
//  Empty string = param was completely absent from the cmdline.
//  Deleted by restore_grub_cmdline() after a successful restore so
//  the next Apply captures a fresh baseline.
#define GRUB_PRECONFIG "/var/lib/ubenchmon/grub_preconfig.json"

// Extract the core-list value of a cmdline param.
// e.g. cmdline="... nohz_full=2,3,4,5 ..." key="nohz_full" → "2,3,4,5"
// Returns empty string (out[0]=='\0') if the param is absent.
static void extract_cmdline_param(const char *cmdline, const char *key,
                                  char *out, size_t out_len)
{
    out[0] = '\0';
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(cmdline, needle);
    if (!p) return;
    p += strlen(needle);
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\n' && i < out_len - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

// Save the current nohz_full/rcu_nocbs state exactly once.
// Guard: if the file already exists we do NOT overwrite — that would
// replace the true pre-ubenchmon baseline with a post-setup state.
static void save_grub_preconfig(void)
{
    struct stat st;
    if (stat(GRUB_PRECONFIG, &st) == 0) return; // already saved

    char cmdline[2048] = {0};
    ubenchmon_read_sysfs_str("/proc/cmdline", cmdline, sizeof(cmdline));

    char nohz[256] = {0};
    char rcu[256]  = {0};
    extract_cmdline_param(cmdline, "nohz_full", nohz, sizeof(nohz));
    extract_cmdline_param(cmdline, "rcu_nocbs",  rcu,  sizeof(rcu));

    ubenchmon_exec("mkdir -p /var/lib/ubenchmon", NULL, 0);

    // Store as JSON strings.  Empty string = param was absent.
    // This is the canonical restore target for teardown.
    char json[512];
    snprintf(json, sizeof(json),
             "{\n"
             "  \"nohz_full_cores\": \"%s\",\n"
             "  \"rcu_nocbs_cores\": \"%s\"\n"
             "}\n",
             nohz, rcu);

    FILE *fp = fopen(GRUB_PRECONFIG, "w");
    if (fp) { fputs(json, fp); fclose(fp); }
}

// Read a JSON string field from a small JSON file.
// Returns 0 on success, -1 if not found.
static int read_json_str(const char *buf, const char *key,
                         char *out, size_t out_len)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(buf, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return -1;
    size_t len = (size_t)(e - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

// Restore GRUB to its pre-ubenchmon state and regenerate grub.cfg.
//
// Algorithm:
//   1. Strip ALL nohz_full= and rcu_nocbs= occurrences from GRUB.
//      (Unconditional — removes whatever ubenchmon may have written.)
//   2. If the saved value was non-empty, re-inject it.
//      If the saved value was empty (param was absent), do nothing —
//      the strip step already removed it completely.
//   3. Regenerate grub.cfg and delete the preconfig file.
static void restore_grub_cmdline(void)
{
    FILE *fp = fopen(GRUB_PRECONFIG, "r");
    if (!fp) return; // nothing to restore

    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    char nohz[256] = {0};
    char rcu[256]  = {0};
    read_json_str(buf, "nohz_full_cores", nohz, sizeof(nohz));
    read_json_str(buf, "rcu_nocbs_cores",  rcu,  sizeof(rcu));

    const char *gp = "/etc/default/grub";
    struct stat st;
    if (stat(gp, &st) != 0) { remove(GRUB_PRECONFIG); return; }

    ubenchmon_exec("cp /etc/default/grub /etc/default/grub.benchmon.prerestore.bak",
                  NULL, 0);

    // Step 1: strip both params completely
    ubenchmon_exec(
        "sed -i 's/ nohz_full=[^ \"]*//g; s/ rcu_nocbs=[^ \"]*//g' "
        "/etc/default/grub",
        NULL, 0);

    // Step 2: re-inject original values only if they were present
    if (nohz[0] != '\0') {
        char sed[512];
        snprintf(sed, sizeof(sed),
                 "sed -i 's|^GRUB_CMDLINE_LINUX=\"\\(.*\\)\"|"
                 "GRUB_CMDLINE_LINUX=\"\\1 nohz_full=%s\"|' "
                 "/etc/default/grub",
                 nohz);
        ubenchmon_exec(sed, NULL, 0);
    }
    if (rcu[0] != '\0') {
        char sed[512];
        snprintf(sed, sizeof(sed),
                 "sed -i 's|^GRUB_CMDLINE_LINUX=\"\\(.*\\)\"|"
                 "GRUB_CMDLINE_LINUX=\"\\1 rcu_nocbs=%s\"|' "
                 "/etc/default/grub",
                 rcu);
        ubenchmon_exec(sed, NULL, 0);
    }

    // Step 3: regenerate
    ubenchmon_exec("update-grub 2>/dev/null || "
                  "grub2-mkconfig -o /boot/grub2/grub.cfg 2>/dev/null || "
                  "grub-mkconfig -o /boot/grub/grub.cfg 2>/dev/null",
                  NULL, 0);

    remove(GRUB_PRECONFIG);
}


//  GRUB / kernel boot parameters
static int setup_grub(const ubenchmon_setup_config_t *cfg,
                      ubenchmon_setup_result_t *res)
{
    if (!cfg->isolated_cores || cfg->isolated_cores_count == 0) return 0;

    char cores[256] = {0};
    for (int i = 0; i < cfg->isolated_cores_count; i++) {
        char t[16];
        snprintf(t, sizeof(t), "%s%d", i ? "," : "", cfg->isolated_cores[i]);
        strncat(cores, t, sizeof(cores) - strlen(cores) - 1);
    }

    // Save baseline BEFORE we touch GRUB for the first time
    save_grub_preconfig();

    const char *gp = "/etc/default/grub";
    struct stat st;
    if (stat(gp, &st) != 0) {
        result_append(res, "GRUB: %s not found", gp);
        return -1;
    }

    // Regardless of what we are about to write, first strip any
    // previously-applied nohz_full/rcu_nocbs so we start clean.
    // This handles the case where the user toggled the flags off —
    // we need to actively remove them, not just skip adding them.
    ubenchmon_exec(
        "sed -i 's/ nohz_full=[^ \"]*//g; s/ rcu_nocbs=[^ \"]*//g' "
        "/etc/default/grub",
        NULL, 0);

    // Build the new param string for isolcpus (always applied)
    char nohz_part[128] = {0};
    char rcu_part[128]  = {0};
    if (cfg->apply_nohz_full)
        snprintf(nohz_part, sizeof(nohz_part), " nohz_full=%s", cores);
    if (cfg->apply_rcu_nocbs)
        snprintf(rcu_part, sizeof(rcu_part), " rcu_nocbs=%s", cores);

    char params[512];
    snprintf(params, sizeof(params),
             "isolcpus=%s%s%s processor.max_cstate=%d nosoftlockup",
             cores, nohz_part, rcu_part,
             cfg->max_cstate >= 0 ? cfg->max_cstate : 0);

    // Check if these exact params are already active in the running kernel
    char cmdline[2048] = {0};
    ubenchmon_read_sysfs_str("/proc/cmdline", cmdline, sizeof(cmdline));

    int isolcpus_ok = (strstr(cmdline, "isolcpus=") && strstr(cmdline, cores)) ? 1 : 0;
    int nohz_ok     = !cfg->apply_nohz_full ||
                      (strstr(cmdline, "nohz_full=") && strstr(cmdline, cores));
    int rcu_ok      = !cfg->apply_rcu_nocbs  ||
                      (strstr(cmdline, "rcu_nocbs=")  && strstr(cmdline, cores));

    if (isolcpus_ok && nohz_ok && rcu_ok) {
        result_append(res, "GRUB: params already active (isolcpus=%s%s%s)",
                      cores,
                      cfg->apply_nohz_full ? " nohz_full=<cores>" : "",
                      cfg->apply_rcu_nocbs  ? " rcu_nocbs=<cores>"  : "");
        return 0;
    }

    ubenchmon_exec("cp /etc/default/grub /etc/default/grub.benchmon.bak",
                  NULL, 0);

    // Inject isolcpus (+ nohz/rcu if enabled).
    // Also strip any stale isolcpus before re-adding so we never duplicate.
    ubenchmon_exec(
        "sed -i 's/ isolcpus=[^ \"]*//g; "
                 "s/ processor\\.max_cstate=[^ \"]*//g; "
                 "s/ nosoftlockup//g' /etc/default/grub",
        NULL, 0);

    char sed[2048];
    snprintf(sed, sizeof(sed),
             "grep -q '^GRUB_CMDLINE_LINUX=' %s && "
             "sed -i 's|^GRUB_CMDLINE_LINUX=\"\\(.*\\)\"|"
             "GRUB_CMDLINE_LINUX=\"\\1 %s\"|' %s || "
             "echo 'GRUB_CMDLINE_LINUX=\"%s\"' >> %s",
             gp, params, gp, params, gp);
    ubenchmon_exec(sed, NULL, 0);

    if (ubenchmon_exec("update-grub 2>/dev/null || "
                      "grub2-mkconfig -o /boot/grub2/grub.cfg 2>/dev/null || "
                      "grub-mkconfig -o /boot/grub/grub.cfg 2>/dev/null",
                      NULL, 0) != 0)
        result_append(res, "GRUB: modified but could not regenerate — "
                     "run update-grub manually");

    res->grub_modified = 1;
    res->reboot_required = 1;
    result_append(res, "GRUB: added [%s] — REBOOT REQUIRED", params);
    return 1;
}

//  SMT — read-only check; must be disabled in BIOS
static void check_smt(ubenchmon_setup_result_t *res)
{
    char buf[16] = {0};
    ubenchmon_read_sysfs_str("/sys/devices/system/cpu/smt/active",
                            buf, sizeof(buf));
    if (buf[0] != '0') {
        result_append(res,
            "SMT: WARNING — Hyper-Threading/SMT is ENABLED. "
            "Disable it in BIOS/UEFI for accurate benchmark results. "
            "lscpu should show 'Thread(s) per core: 1'");
    } else {
        result_append(res, "SMT: disabled (OK)");
    }
}

//  Frequency
static int setup_freq(const ubenchmon_setup_config_t *cfg,
                      ubenchmon_setup_result_t *res)
{
    if (cfg->disable_frequency_boost) {
        ubenchmon_write_sysfs_str(
            "/sys/devices/system/cpu/intel_pstate/no_turbo", "1");
        ubenchmon_write_sysfs_str(
            "/sys/devices/system/cpu/cpufreq/boost", "0");
        result_append(res, "FREQ: boost disabled");
    }

    char path[128];
    for (int i = 0; i < 256; i++) {
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        if (ubenchmon_write_sysfs_str(path, "performance") != 0) break;
    }
    res->frequency_locked = 1;
    result_append(res, "FREQ: governor → performance");
    return 0;
}

//  IRQ affinity
static int setup_irq(const ubenchmon_setup_config_t *cfg,
                     ubenchmon_setup_result_t *res)
{
    if (!cfg->isolated_cores || cfg->isolated_cores_count == 0) return 0;

    char mask[32];
    snprintf(mask, sizeof(mask), "%x", 1 << cfg->housekeeping_core);

    ubenchmon_write_sysfs_str("/proc/irq/default_smp_affinity", mask);

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "for f in /proc/irq/[0-9]*/smp_affinity; do "
             "echo '%s' > \"$f\" 2>/dev/null; done", mask);
    ubenchmon_exec(cmd, NULL, 0);

    res->irq_migrated = 1;
    result_append(res, "IRQ: migrated to core %d", cfg->housekeeping_core);
    return 0;
}

//  Services / swap
static void setup_services(const ubenchmon_setup_config_t *cfg,
                           ubenchmon_setup_result_t *res)
{
    if (cfg->stop_irqbalance)
        ubenchmon_exec("systemctl stop irqbalance 2>/dev/null", NULL, 0);
    if (cfg->isolate_multiuser)
        ubenchmon_exec("systemctl isolate multi-user.target 2>/dev/null",
                     NULL, 0);

    const char *noisy[] = {"cron","anacron","atd","snapd",
                           "unattended-upgrades","packagekitd",NULL};
    for (int i = 0; noisy[i]; i++) {
        char c[128];
        snprintf(c, sizeof(c), "systemctl stop %s 2>/dev/null", noisy[i]);
        ubenchmon_exec(c, NULL, 0);
    }
    res->services_stopped = 1;
    result_append(res, "SERVICES: noisy daemons stopped");
}

static void setup_swap(const ubenchmon_setup_config_t *cfg,
                       ubenchmon_setup_result_t *res)
{
    if (!cfg->disable_swap) return;
    ubenchmon_exec("swapoff -a", NULL, 0);
    res->swap_disabled = 1;
    result_append(res, "SWAP: disabled");
}

//  Sysctl preconfig — capture current values before modifying
#define PRECONFIG_SYSCTL "/var/lib/ubenchmon/preconfig_sysctl.json"

static void capture_sysctl_preconfig(void) {
    struct stat st;
    if (stat(PRECONFIG_SYSCTL, &st) == 0) return; // already captured

    long aslr = 2, rmem_max = 212992, wmem_max = 212992;
    long rmem_def = 212992, wmem_def = 212992, backlog = 1000;

    ubenchmon_read_sysfs_int("/proc/sys/kernel/randomize_va_space", &aslr);
    ubenchmon_read_sysfs_int("/proc/sys/net/core/rmem_max",          &rmem_max);
    ubenchmon_read_sysfs_int("/proc/sys/net/core/wmem_max",          &wmem_max);
    ubenchmon_read_sysfs_int("/proc/sys/net/core/rmem_default",      &rmem_def);
    ubenchmon_read_sysfs_int("/proc/sys/net/core/wmem_default",      &wmem_def);
    ubenchmon_read_sysfs_int("/proc/sys/net/core/netdev_max_backlog", &backlog);

    char tstate[32] = {0};
    ubenchmon_exec("systemctl is-active systemd-timesyncd 2>/dev/null",
                  tstate, sizeof(tstate));
    int timesyncd_active = (strncmp(tstate, "active", 6) == 0) ? 1 : 0;

    // Capture irqbalance state
    char irqb_state[32] = {0};
    ubenchmon_exec("systemctl is-active irqbalance 2>/dev/null",
                  irqb_state, sizeof(irqb_state));
    int irqbalance_was_active = (strncmp(irqb_state, "active", 6) == 0) ? 1 : 0;

    // Capture frequency boost state
    long freq_boost = 1;
    if (ubenchmon_read_sysfs_int("/sys/devices/system/cpu/cpufreq/boost",
                                &freq_boost) != 0) {
        long no_turbo = 0;
        if (ubenchmon_read_sysfs_int(
                "/sys/devices/system/cpu/intel_pstate/no_turbo",
                &no_turbo) == 0)
            freq_boost = !no_turbo;
    }

    // Capture CPU governor
    char governor[64] = "ondemand";
    ubenchmon_read_sysfs_str(
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
        governor, sizeof(governor));

    // Capture noisy services state
    const char *noisy[] = {"cron","anacron","atd","snapd",
                           "unattended-upgrades","packagekitd",NULL};
    char noisy_states[512] = {0};
    for (int i = 0; noisy[i]; i++) {
        char cmd[128], out[32] = {0};
        snprintf(cmd, sizeof(cmd),
                 "systemctl is-active %s 2>/dev/null", noisy[i]);
        ubenchmon_exec(cmd, out, sizeof(out));
        if (strncmp(out, "active", 6) == 0) {
            if (noisy_states[0]) strncat(noisy_states, ",",
                sizeof(noisy_states) - strlen(noisy_states) - 1);
            strncat(noisy_states, noisy[i],
                sizeof(noisy_states) - strlen(noisy_states) - 1);
        }
    }

    // Capture systemd target (graphical vs multi-user)
    char target[64] = {0};
    ubenchmon_exec("systemctl get-default 2>/dev/null", target, sizeof(target));

    ubenchmon_exec("mkdir -p /var/lib/ubenchmon", NULL, 0);

    char json[2048];
    snprintf(json, sizeof(json),
             "{\n"
             "  \"aslr\": %ld,\n"
             "  \"rmem_max\": %ld,\n"
             "  \"wmem_max\": %ld,\n"
             "  \"rmem_default\": %ld,\n"
             "  \"wmem_default\": %ld,\n"
             "  \"netdev_max_backlog\": %ld,\n"
             "  \"timesyncd_was_active\": %s,\n"
             "  \"irqbalance_was_active\": %s,\n"
             "  \"freq_boost_was_on\": %s,\n"
             "  \"governor\": \"%s\",\n"
             "  \"noisy_services_active\": \"%s\",\n"
             "  \"default_target\": \"%s\"\n"
             "}\n",
             aslr, rmem_max, wmem_max,
             rmem_def, wmem_def, backlog,
             timesyncd_active ? "true" : "false",
             irqbalance_was_active ? "true" : "false",
             freq_boost ? "true" : "false",
             governor,
             noisy_states,
             target);

    FILE *fp = fopen(PRECONFIG_SYSCTL, "w");
    if (fp) {
        fputs(json, fp);
        fclose(fp);
    }
}

static void restore_sysctl_preconfig(void) {
    FILE *fp = fopen(PRECONFIG_SYSCTL, "r");
    if (!fp) return;

    char buf[2048] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    #define EXTRACT_INT(key, dfl) ({ \
        long _v = (dfl); \
        char *_p = strstr(buf, "\"" key "\":"); \
        if (_p) { _p += strlen("\"" key "\":"); while (*_p == ' ') _p++; _v = strtol(_p, NULL, 10); } \
        _v; \
    })

    long aslr     = EXTRACT_INT("aslr",                   2);
    long rmem_max = EXTRACT_INT("rmem_max",           212992);
    long wmem_max = EXTRACT_INT("wmem_max",           212992);
    long rmem_def = EXTRACT_INT("rmem_default",       212992);
    long wmem_def = EXTRACT_INT("wmem_default",       212992);
    long backlog  = EXTRACT_INT("netdev_max_backlog",   1000);

    #undef EXTRACT_INT

    // Helper: extract bool from JSON
    #define EXTRACT_BOOL(key) ({ \
        int _b = 0; \
        char *_p = strstr(buf, "\"" key "\":"); \
        if (_p) { _p += strlen("\"" key "\":"); while (*_p == ' ') _p++; \
                  _b = (strncmp(_p, "true", 4) == 0); } \
        _b; \
    })

    int timesyncd_was_active    = EXTRACT_BOOL("timesyncd_was_active");
    int irqbalance_was_active   = EXTRACT_BOOL("irqbalance_was_active");
    int freq_boost_was_on       = EXTRACT_BOOL("freq_boost_was_on");

    #undef EXTRACT_BOOL

    // Extract governor string
    char governor[64] = "ondemand";
    {
        char *p = strstr(buf, "\"governor\":");
        if (p) {
            p += strlen("\"governor\":");
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                p++;
                char *e = strchr(p, '"');
                if (e) {
                    size_t l = (size_t)(e - p);
                    if (l >= sizeof(governor)) l = sizeof(governor) - 1;
                    memcpy(governor, p, l);
                    governor[l] = '\0';
                }
            }
        }
    }

    // Extract noisy_services_active string
    char noisy_services[512] = {0};
    {
        char *p = strstr(buf, "\"noisy_services_active\":");
        if (p) {
            p += strlen("\"noisy_services_active\":");
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                p++;
                char *e = strchr(p, '"');
                if (e) {
                    size_t l = (size_t)(e - p);
                    if (l >= sizeof(noisy_services)) l = sizeof(noisy_services) - 1;
                    memcpy(noisy_services, p, l);
                    noisy_services[l] = '\0';
                }
            }
        }
    }

    char cmd[256];

    // Restore sysctl values
    snprintf(cmd, sizeof(cmd),
             "sysctl -w kernel.randomize_va_space=%ld 2>/dev/null", aslr);
    ubenchmon_exec(cmd, NULL, 0);
    snprintf(cmd, sizeof(cmd),
             "sysctl -w net.core.rmem_max=%ld 2>/dev/null", rmem_max);
    ubenchmon_exec(cmd, NULL, 0);
    snprintf(cmd, sizeof(cmd),
             "sysctl -w net.core.wmem_max=%ld 2>/dev/null", wmem_max);
    ubenchmon_exec(cmd, NULL, 0);
    snprintf(cmd, sizeof(cmd),
             "sysctl -w net.core.rmem_default=%ld 2>/dev/null", rmem_def);
    ubenchmon_exec(cmd, NULL, 0);
    snprintf(cmd, sizeof(cmd),
             "sysctl -w net.core.wmem_default=%ld 2>/dev/null", wmem_def);
    ubenchmon_exec(cmd, NULL, 0);
    snprintf(cmd, sizeof(cmd),
             "sysctl -w net.core.netdev_max_backlog=%ld 2>/dev/null", backlog);
    ubenchmon_exec(cmd, NULL, 0);

    // Restore timesyncd
    if (timesyncd_was_active)
        ubenchmon_exec("systemctl start systemd-timesyncd 2>/dev/null", NULL, 0);

    // Restore irqbalance
    if (irqbalance_was_active)
        ubenchmon_exec("systemctl start irqbalance 2>/dev/null", NULL, 0);

    // Restore frequency boost
    if (freq_boost_was_on) {
        ubenchmon_write_sysfs_str(
            "/sys/devices/system/cpu/cpufreq/boost", "1");
        ubenchmon_write_sysfs_str(
            "/sys/devices/system/cpu/intel_pstate/no_turbo", "0");
    }

    // Restore CPU governor on all cores
    if (governor[0]) {
        char path[128];
        for (int i = 0; i < 256; i++) {
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
            if (ubenchmon_write_sysfs_str(path, governor) != 0) break;
        }
    }

    // Restore IRQ affinity to all cores
    ubenchmon_exec(
        "for f in /proc/irq/[0-9]*/smp_affinity; do "
        "echo 'ffffffff' > \"$f\" 2>/dev/null; done", NULL, 0);
    ubenchmon_write_sysfs_str("/proc/irq/default_smp_affinity", "ffffffff");

    // Restart noisy services that were previously active
    if (noisy_services[0]) {
        char svc_buf[512];
        strncpy(svc_buf, noisy_services, sizeof(svc_buf) - 1);
        svc_buf[sizeof(svc_buf) - 1] = '\0';
        char *tok = strtok(svc_buf, ",");
        while (tok) {
            snprintf(cmd, sizeof(cmd),
                     "systemctl start %s 2>/dev/null", tok);
            ubenchmon_exec(cmd, NULL, 0);
            tok = strtok(NULL, ",");
        }
    }

    remove(PRECONFIG_SYSCTL);
}


//  Sysctl tuning
static void setup_sysctl(const ubenchmon_setup_config_t *cfg,
                         ubenchmon_setup_result_t *res)
{
    int did = 0;

    if (cfg->disable_aslr) {
        ubenchmon_exec("sysctl -w kernel.randomize_va_space=0 2>/dev/null",
                      NULL, 0);
        result_append(res, "SYSCTL: ASLR disabled");
        did = 1;
    }
    if (cfg->tune_net_buffers) {
        ubenchmon_exec("sysctl -w net.core.rmem_max=26214400 2>/dev/null",
                      NULL, 0);
        ubenchmon_exec("sysctl -w net.core.wmem_max=26214400 2>/dev/null",
                      NULL, 0);
        ubenchmon_exec("sysctl -w net.core.rmem_default=1048576 2>/dev/null",
                      NULL, 0);
        ubenchmon_exec("sysctl -w net.core.wmem_default=1048576 2>/dev/null",
                      NULL, 0);
        ubenchmon_exec("sysctl -w net.core.netdev_max_backlog=5000 2>/dev/null",
                      NULL, 0);
        result_append(res, "SYSCTL: net buffers tuned");
        did = 1;
    }
    if (cfg->stop_timesyncd) {
        ubenchmon_exec("systemctl stop systemd-timesyncd 2>/dev/null", NULL, 0);
        ubenchmon_exec("systemctl stop NetworkManager-wait-online 2>/dev/null",
                      NULL, 0);
        result_append(res, "SYSCTL: timesyncd stopped");
        did = 1;
    }
    if (cfg->drop_caches) {
        ubenchmon_exec("sync && echo 3 > /proc/sys/vm/drop_caches 2>/dev/null",
                      NULL, 0);
        res->caches_dropped = 1;
        result_append(res, "CACHE: page cache dropped");
        did = 1;
    }
    if (did) res->sysctl_tuned = 1;
}


//  Process isolation config
static void setup_process_isolation(const ubenchmon_setup_config_t *cfg,
                                    ubenchmon_setup_result_t *res)
{
    if ((!cfg->server_cores || cfg->server_cores[0] == '\0') &&
        cfg->rt_priority == 0) return;

    if (cfg->server_cores && cfg->server_cores[0] != '\0') {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "taskset -c %s true 2>/dev/null",
                 cfg->server_cores);
        if (ubenchmon_exec(cmd, NULL, 0) != 0) {
            result_append(res,
                "PROC: WARNING — server_cores '%s' invalid",
                cfg->server_cores);
            return;
        }
    }
    if (cfg->client_cores && cfg->client_cores[0] != '\0') {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "taskset -c %s true 2>/dev/null",
                 cfg->client_cores);
        if (ubenchmon_exec(cmd, NULL, 0) != 0) {
            result_append(res,
                "PROC: WARNING — client_cores '%s' invalid",
                cfg->client_cores);
            return;
        }
    }

    res->process_isolation_ready = 1;
    result_append(res, "PROC: server=%s client=%s rt=%d",
        cfg->server_cores ? cfg->server_cores : "(unset)",
        cfg->client_cores ? cfg->client_cores : "(unset)",
        cfg->rt_priority);
}


//  Network namespaces + veth + netem
static int setup_network(const ubenchmon_setup_config_t *cfg,
                         ubenchmon_setup_result_t *res)
{
    if (!cfg->ns_server_name || !cfg->ns_client_name) return 0;

    const char *ns_s = cfg->ns_server_name;
    const char *ns_c = cfg->ns_client_name;
    const char *ve_s = cfg->veth_server_name ? cfg->veth_server_name : "veth-s";
    const char *ve_c = cfg->veth_client_name ? cfg->veth_client_name : "veth-c";
    const char *ip_s = cfg->server_ip        ? cfg->server_ip        : "10.0.0.1/24";
    const char *ip_c = cfg->client_ip        ? cfg->client_ip        : "10.0.0.2/24";

    char cmd[1024];

    // Kill any lingering sleep processes from previous setup
    snprintf(cmd, sizeof(cmd),
             "pkill -f 'ip netns exec %s sleep' 2>/dev/null; "
             "pkill -f 'ip netns exec %s sleep' 2>/dev/null",
             ns_s, ns_c);
    ubenchmon_exec(cmd, NULL, 0);

    // Idempotent cleanup
    snprintf(cmd, sizeof(cmd),
             "ip netns del %s 2>/dev/null; ip netns del %s 2>/dev/null; "
             "ip link del %s 2>/dev/null", ns_s, ns_c, ve_s);
    ubenchmon_exec(cmd, NULL, 0);

    // Create namespaces + veth
    snprintf(cmd, sizeof(cmd),
             "ip netns add %s && ip netns add %s && "
             "ip link add %s type veth peer name %s && "
             "ip link set %s netns %s && ip link set %s netns %s",
             ns_s, ns_c, ve_s, ve_c, ve_s, ns_s, ve_c, ns_c);
    if (ubenchmon_exec(cmd, NULL, 0) != 0) {
        result_append(res, "NET: namespace/veth creation failed");
        return -1;
    }

    // IP + up (server)
    snprintf(cmd, sizeof(cmd),
             "ip netns exec %s sh -c '"
             "ip addr add %s dev %s && ip link set %s up && ip link set lo up'",
             ns_s, ip_s, ve_s, ve_s);
    ubenchmon_exec(cmd, NULL, 0);

    // IP + up (client)
    snprintf(cmd, sizeof(cmd),
             "ip netns exec %s sh -c '"
             "ip addr add %s dev %s && ip link set %s up && ip link set lo up'",
             ns_c, ip_c, ve_c, ve_c);
    ubenchmon_exec(cmd, NULL, 0);

    snprintf(cmd, sizeof(cmd),
            "ip netns exec %s ip link set %s mtu 1500 && "
            "ip netns exec %s ip link set %s mtu 1500",
            ns_s, ve_s, ns_c, ve_c);
    ubenchmon_exec(cmd, NULL, 0);

    res->namespaces_created = 1;
    result_append(res, "NET: %s/%s ← veth %s↔%s", ns_s, ns_c, ve_s, ve_c);

    // Disable offloading
    if (cfg->disable_offloading) {
        snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ethtool -K %s tx off rx off tso off gso off gro off 2>/dev/null && "
                 "ip netns exec %s ethtool -K %s tx off rx off tso off gso off gro off 2>/dev/null",
                 ns_s, ve_s, ns_c, ve_c);
        if (ubenchmon_exec(cmd, NULL, 0) == 0) {
            res->offloading_disabled = 1;
            result_append(res, "NET: tx/rx/TSO/GSO/GRO off");
        }
    }

    // NetEm
    if (cfg->netem_delay_ms > 0 || cfg->netem_loss_pct > 0) {
        char args[256] = {0};
        if (cfg->netem_delay_ms > 0) {
            char t[64];
            snprintf(t, sizeof(t), "delay %dms", cfg->netem_delay_ms);
            strncat(args, t, sizeof(args) - strlen(args) - 1);
            if (cfg->netem_jitter_ms > 0) {
                snprintf(t, sizeof(t), " %dms distribution normal",
                         cfg->netem_jitter_ms);
                strncat(args, t, sizeof(args) - strlen(args) - 1);
            }
        }
        if (cfg->netem_loss_pct > 0) {
            char t[64];
            snprintf(t, sizeof(t), " loss %.2f%%", cfg->netem_loss_pct);
            strncat(args, t, sizeof(args) - strlen(args) - 1);
        }

        snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s tc qdisc add dev %s root netem %s && "
                 "ip netns exec %s tc qdisc add dev %s root netem %s",
                 ns_s, ve_s, args, ns_c, ve_c, args);
        if (ubenchmon_exec(cmd, NULL, 0) == 0) {
            res->netem_applied = 1;
            result_append(res, "NET: netem [%s]", args);
        }
    }

    // Start a long-lived process inside each namespace so that
    // monitor.c can open veth stats via /proc/<pid>/root/sys/...
    // without needing setns() (which fails in multithreaded processes).
    snprintf(cmd, sizeof(cmd),
             "ip netns exec %s sleep infinity </dev/null >/dev/null 2>&1 &",
             ns_s);
    ubenchmon_exec(cmd, NULL, 0);

    snprintf(cmd, sizeof(cmd),
             "ip netns exec %s sleep infinity </dev/null >/dev/null 2>&1 &",
             ns_c);
    ubenchmon_exec(cmd, NULL, 0);

    result_append(res, "NET: monitor anchors started in %s and %s",
                  ns_s, ns_c);

    return 0;
}

//  Public: ubenchmon_setup()
ubenchmon_status_t ubenchmon_setup(const ubenchmon_setup_config_t *cfg,
                                 ubenchmon_setup_result_t *res)
{
    memset(res, 0, sizeof(*res));

    if (!is_root()) {
        res->status = UBENCHMON_ERR_PERM;
        snprintf(res->message, sizeof(res->message),
                 "ubenchmon_setup requires root");
        return UBENCHMON_ERR_PERM;
    }

    int err = 0;
    capture_sysctl_preconfig();
    check_smt(res);
    if (setup_grub(cfg, res)    < 0) err++;
    setup_freq(cfg, res);
    if (setup_irq(cfg, res)     < 0) err++;
    setup_services(cfg, res);
    setup_swap(cfg, res);
    setup_sysctl(cfg, res);
    setup_process_isolation(cfg, res);
    if (setup_network(cfg, res) < 0) err++;

    res->status = res->reboot_required ? UBENCHMON_ERR_REBOOT
                : err                  ? UBENCHMON_ERR_PARTIAL
                :                        UBENCHMON_OK;
    return res->status;
}

//  Public: ubenchmon_teardown()
ubenchmon_status_t ubenchmon_teardown(const ubenchmon_setup_config_t *cfg) {
    if (!is_root()) return UBENCHMON_ERR_PERM;

    // Restore GRUB to its exact pre-ubenchmon state.
    // This handles all cases:
    //   - nohz_full/rcu_nocbs were absent before → they get stripped
    //   - they were present with different cores  → they get restored verbatim
    //   - they were present with same cores       → they get restored verbatim
    restore_grub_cmdline();

    // Kill monitor anchor processes
    if (cfg->ns_server_name) {
        char c[256];
        snprintf(c, sizeof(c),
                 "pkill -f 'ip netns exec %s sleep' 2>/dev/null",
                 cfg->ns_server_name);
        ubenchmon_exec(c, NULL, 0);

        snprintf(c, sizeof(c), "ip netns del %s 2>/dev/null",
                 cfg->ns_server_name);
        ubenchmon_exec(c, NULL, 0);
    }
    if (cfg->ns_client_name) {
        char c[256];
        snprintf(c, sizeof(c),
                 "pkill -f 'ip netns exec %s sleep' 2>/dev/null",
                 cfg->ns_client_name);
        ubenchmon_exec(c, NULL, 0);

        snprintf(c, sizeof(c), "ip netns del %s 2>/dev/null",
                 cfg->ns_client_name);
        ubenchmon_exec(c, NULL, 0);
    }

    // Restore swap
    ubenchmon_exec("swapon -a 2>/dev/null", NULL, 0);

    // Restore sysctl, services, freq boost, governor, IRQ affinity
    restore_sysctl_preconfig();

    // ---- Clean up all ubenchmon state files ----
    // Remove GRUB backup files
    ubenchmon_exec("rm -f /etc/default/grub.ubenchmon.bak 2>/dev/null",
                  NULL, 0);
    ubenchmon_exec("rm -f /etc/default/grub.ubenchmon.prerestore.bak 2>/dev/null",
                  NULL, 0);

    // Remove runtime state directory
    ubenchmon_exec("rm -rf /var/run/ubenchmon 2>/dev/null", NULL, 0);

    // Remove log files
    ubenchmon_exec("rm -rf /var/log/ubenchmon 2>/dev/null", NULL, 0);

    // Remove all state (snapshots, active config, preconfig files)
    ubenchmon_exec("rm -rf /var/lib/ubenchmon 2>/dev/null", NULL, 0);

    return UBENCHMON_OK;
}

//  Public: ubenchmon_get_launch_prefix()
char *ubenchmon_get_launch_prefix(const ubenchmon_setup_config_t *cfg,
                                 int is_server)
{
    char buf[256] = {0};
    const char *cores = is_server ? cfg->server_cores : cfg->client_cores;

    if (cores && cores[0] != '\0' && cfg->rt_priority > 0)
        snprintf(buf, sizeof(buf), "taskset -c %s chrt -f %d ",
                 cores, cfg->rt_priority);
    else if (cores && cores[0] != '\0')
        snprintf(buf, sizeof(buf), "taskset -c %s ", cores);
    else if (cfg->rt_priority > 0)
        snprintf(buf, sizeof(buf), "chrt -f %d ", cfg->rt_priority);

    char *out = (char *)malloc(strlen(buf) + 1);
    if (out) strcpy(out, buf);
    return out;
}
