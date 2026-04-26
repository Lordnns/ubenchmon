//
// utils.c — Shared utility functions
//

#define _GNU_SOURCE
#include "ubenchmon_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

__thread char ubenchmon_errbuf[UBENCHMON_ERRBUF_SIZE] = {0};

const char *ubenchmon_strerror(void) {
    return ubenchmon_errbuf;
}

int ubenchmon_exec(const char *cmd, char *out, size_t out_size) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        UBENCHMON_SET_ERR_ERRNO("popen");
        return -1;
    }

    if (out && out_size > 0) {
        out[0] = '\0';
        size_t total = 0;
        while (total < out_size - 1) {
            size_t n = fread(out + total, 1, out_size - 1 - total, fp);
            if (n == 0) break;
            total += n;
        }
        out[total] = '\0';
        // Strip trailing newline
        if (total > 0 && out[total - 1] == '\n')
            out[total - 1] = '\0';
    } else {
        // Drain output
        char buf[256];
        while (fread(buf, 1, sizeof(buf), fp) > 0) {}
    }

    int status = pclose(fp);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int ubenchmon_read_sysfs_int(const char *path, long *value) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        UBENCHMON_SET_ERR("Cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        UBENCHMON_SET_ERR("Cannot read %s", path);
        return -1;
    }
    buf[n] = '\0';
    *value = strtol(buf, NULL, 10);
    return 0;
}

int ubenchmon_read_sysfs_str(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        UBENCHMON_SET_ERR("Cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    ssize_t n = read(fd, buf, len - 1);
    close(fd);

    if (n <= 0) {
        UBENCHMON_SET_ERR("Cannot read %s", path);
        return -1;
    }
    buf[n] = '\0';
    // Strip trailing newline
    if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = '\0';
    return 0;
}

int ubenchmon_write_sysfs_str(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        UBENCHMON_SET_ERR("Cannot open %s for writing: %s",
                         path, strerror(errno));
        return -1;
    }

    ssize_t n = write(fd, value, strlen(value));
    close(fd);

    if (n < 0) {
        UBENCHMON_SET_ERR("Cannot write to %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}
