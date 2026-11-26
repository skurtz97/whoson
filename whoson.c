/*
 * SPDX-License-Identifier: MIT OR Apache-2.0 OR BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Sean Kurtz
 *
 * whoson - who is logged on to this z/OS system, and how (TSO/E or SSH).
 *
 * Sessions come from the address space control blocks, not from a subsystem
 * or a console command: CVT (PSA+X'10') -> ASVT -> every allocated ASCB. That
 * walk reads only common storage, which is mapped into every address space,
 * so the program needs no APF authorization, no supervisor state and no key
 * zero. ASCBJBNI holds the job name, ASCBJBNS the name the address space was
 * started with - for a TSO/E session that is the logon procedure - and a
 * non-zero ASCBTSB marks a TSO/E session.
 *
 * Process details (pid, user, command, elapsed time) come from
 * "ps -A -o pid,jobname,user,time,args", joined on the job name. That join is
 * what tells an SSH login from a started task: sshd forks one address space
 * per connection and the fork inherits the job name of the connection.
 *
 * The ASCB CPU accumulators, ASCBEJST and ASCBSRBT, count in 1024-microsecond
 * units, so the CPU column is printed as seconds.
 *
 * Ctrl blocks are read through volatile pointers on purpose: nothing in this
 * file may be cached across the walk, because the blocks change under it.
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SPACES 1024
#define MAX_PROCS 8192
#define MAX_TOKENS 16
#define MSG_LEN 512
#define NAME_LEN 16
#define JOB_LEN 9
#define ASID_LEN 8
#define PID_LEN 12
#define CPU_LEN 16
#define CMD_LEN 256
#define TIME_LEN 16

/* ASCB field offsets (mvs-data-areas, checked against IHAASCB on the system). */
#define ASCB_ASID 0x24u
#define ASCB_TSB  0x3Cu
#define ASCB_EJST 0x40u
#define ASCB_JBNI 0xACu
#define ASCB_JBNS 0xB0u
#define ASCB_SRBT 0xC8u

/* CVTASVT, and the ASVT's own layout: the array of ASCB pointers starts well
 * past the head of the block, and a set high bit marks a free ASID. */
#define CVT_ASVT 556u
#define ASVT_MAXU 0x204u
#define ASVT_ENTY 0x210u
#define ASCB_ACRONYM 0xC1E2C3C2u /* "ASCB" in EBCDIC */

struct space {
    unsigned asid;
    int tso;
    unsigned long long cpu;
    char job[JOB_LEN];
    char logon[JOB_LEN];
    char user[NAME_LEN];
    char type[NAME_LEN];
    int32_t pid;
};

struct proc {
    int32_t pid;
    char job[JOB_LEN];
    char user[NAME_LEN];
    char time[TIME_LEN];
    char cmd[CMD_LEN];
};

static const char usage_text[] =
    "usage: whoson [--help | --version]\n"
    "\n"
    "List the interactive sessions on this system: TSO/E, z/OSMF (which is a\n"
    "TSO/E session started by the z/OSMF logon procedure) and SSH. Started\n"
    "tasks, OMVS daemons and the processes inside a session are not listed.\n"
    "\n"
    "Options:\n"
    "  -h, --help     show this help and exit\n"
    "  -V, --version  show the version and exit\n";

/* Set by the Makefile from its VERSION, so the binary and the tag agree. */
#ifndef WHOSON_VERSION
#define WHOSON_VERSION "unknown"
#endif

/* Write a message in the character set its destination expects. CEE converts
 * stdio output from ASCII to EBCDIC when the stream is a terminal, and sshd
 * converts a pipe, socket or file the other way, so neither printf nor a raw
 * write() is right on both: a terminal gets ASCII, everything else EBCDIC. */
static void emit(int fd, char *buf, size_t len) {
    if (!isatty(fd))
        __a2e_l(buf, len);
    while (len > 0) {
        ssize_t n = write(fd, buf, len);

        if (n <= 0)
            return;
        buf += n;
        len -= (size_t)n;
    }
}

static void emit_str(int fd, const char *text) {
    char buf[MSG_LEN];
    size_t len = strlen(text);

    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    emit(fd, buf, len);
}

/* Report an error on stderr; messages are short enough for one line buffer. */
static void error_msg(const char *fmt, ...) {
    char text[MSG_LEN];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    emit_str(STDERR_FILENO, text);
}

static void usage_error(const char *fmt, ...) {
    char text[MSG_LEN];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    emit_str(STDERR_FILENO, text);
    emit_str(STDERR_FILENO, "\n");
    emit_str(STDERR_FILENO, usage_text);
}

static void copy_field(char *dst, size_t dstlen, const char *src) {
    snprintf(dst, dstlen, "%s", src);
}


/* Copy the 8-byte name a control block points at. These pointers address
 * common storage, so no cross-memory services are needed to read them. */
static void copy_name(char *dst, uint32_t ptr) {
    char raw[JOB_LEN];
    int i;

    dst[0] = '\0';
    if (ptr == 0)
        return;
    for (i = 0; i < JOB_LEN - 1; i++)
        raw[i] = ((const volatile char *)(uintptr_t)ptr)[i];
    __e2a_l(raw, JOB_LEN - 1); /* names sit in storage as EBCDIC */
    for (i = 0; i < JOB_LEN - 1; i++) {
        if (raw[i] == '\0' || raw[i] == ' ')
            break;
        dst[i] = raw[i];
    }
    dst[i] = '\0';
}

/* Walk the ASVT and describe every allocated address space. */
static int ascb_scan(struct space *sp, int max) {
    uint32_t cvt = *(volatile uint32_t *)(uintptr_t)16;
    uint32_t asvt;
    uint32_t entries;
    unsigned maxu;
    unsigned i;
    int n = 0;

    if (cvt == 0)
        return 0;
    asvt = *(volatile uint32_t *)(uintptr_t)(cvt + CVT_ASVT);
    if (asvt == 0)
        return 0;
    maxu = *(volatile uint32_t *)(uintptr_t)(asvt + ASVT_MAXU) & 0xFFFFu;
    entries = asvt + ASVT_ENTY;
    for (i = 0; i < maxu && n < max; i++) {
        uint32_t ent = *(volatile uint32_t *)(uintptr_t)(entries + 4u * i);
        uint32_t ascb;

        if ((ent & 0x80000000u) != 0)
            continue;
        ascb = ent & 0x7FFFFFFFu;
        if (*(volatile uint32_t *)(uintptr_t)ascb != ASCB_ACRONYM)
            continue;
        sp[n].asid = *(volatile uint16_t *)(uintptr_t)(ascb + ASCB_ASID);
        sp[n].tso = *(volatile uint32_t *)(uintptr_t)(ascb + ASCB_TSB) != 0;
        sp[n].cpu = *(volatile unsigned long long *)(uintptr_t)(ascb + ASCB_EJST) +
                    *(volatile unsigned long long *)(uintptr_t)(ascb + ASCB_SRBT);
        copy_name(sp[n].job, *(volatile uint32_t *)(uintptr_t)(ascb + ASCB_JBNI));
        copy_name(sp[n].logon, *(volatile uint32_t *)(uintptr_t)(ascb + ASCB_JBNS));
        if (sp[n].job[0] == '\0')
            copy_field(sp[n].job, JOB_LEN, sp[n].logon);
        sp[n].user[0] = '\0';
        sp[n].type[0] = '\0';
        sp[n].pid = 0;
        n++;
    }
    return n;
}

/* Read the process table. ps prints one line per process as
 * "pid jobname user time command", so the job name joins to an address
 * space by name and the command tells an sshd session from the listener. */
static int ps_scan(struct proc *pr, int max) {
    FILE *fp;
    char line[512];
    int n = 0;

    fp = popen("ps -A -o pid,jobname,user,time,args", "r");
    if (fp == NULL)
        return 0;
    while (fgets(line, sizeof(line), fp) != NULL && n < max) {
        int32_t pid;
        char job[JOB_LEN];
        char user[NAME_LEN];
        char tm[TIME_LEN];

        job[0] = '\0';
        user[0] = '\0';
        tm[0] = '\0';
        pr[n].cmd[0] = '\0';
        if (sscanf(line, "%" SCNd32 " %8s %15s %15s %255[^\n]", &pid, job, user,
                   tm, pr[n].cmd) < 4)
            continue;
        if (strcmp(job, "JOBNAME") == 0)
            continue;
        pr[n].pid = pid;
        copy_field(pr[n].job, JOB_LEN, job);
        copy_field(pr[n].user, NAME_LEN, user);
        copy_field(pr[n].time, TIME_LEN, tm);
        n++;
    }
    pclose(fp);
    return n;
}


/* An sshd started with -R serves one connection; the listener runs without
 * it, so the flag is what separates a session from the daemon. */
static int is_ssh_session(const struct proc *p) {
    if (strstr(p->cmd, "sshd") == NULL || strstr(p->cmd, " -R") == NULL)
        return 0;
    /* An unauthenticated connection runs as a daemon account; it is not a
     * session until someone has logged on. */
    return strcmp(p->user, "BPXROOT") != 0 && strcmp(p->user, "root") != 0;
}

/* TSO/E sessions come from the ASCB walk, SSH sessions from the process
 * table. Everything else - started tasks, OMVS daemons, fork children of a
 * shell - is not a session and stays out of the list. */
static int collect(struct space *rows, int max, const struct space *sp, int nsp,
                   const struct proc *pr, int npr) {
    int n = 0;
    int i;

    for (i = 0; i < nsp && n < max; i++) {
        if (!sp[i].tso)
            continue;
        rows[n] = sp[i];
        copy_field(rows[n].type, NAME_LEN,
                   strncmp(sp[i].logon, "IZU", 3) == 0 ? "z/OSMF" : "TSO/E");
        copy_field(rows[n].user, NAME_LEN, sp[i].job);
        rows[n].pid = -1;
        n++;
    }
    for (i = 0; i < npr && n < max; i++) {
        unsigned asid = 0;
        unsigned long long cpu = 0;
        int k;
        int dup = 0;

        if (!is_ssh_session(&pr[i]))
            continue;
        for (k = 0; k < nsp; k++) {
            if (strcmp(sp[k].job, pr[i].job) == 0) {
                asid = sp[k].asid;
                cpu = sp[k].cpu;
                break;
            }
        }
        for (k = 0; k < n; k++) {
            if (asid != 0 && rows[k].asid == asid)
                dup = 1;
        }
        if (dup)
            continue;
        rows[n].asid = asid;
        rows[n].cpu = cpu;
        rows[n].pid = pr[i].pid;
        copy_field(rows[n].type, NAME_LEN, "SSH");
        copy_field(rows[n].job, JOB_LEN, pr[i].job);
        copy_field(rows[n].user, NAME_LEN, pr[i].user);
        copy_field(rows[n].logon, JOB_LEN, "-");
        n++;
    }
    return n;
}

static int compare_rows(const void *a, const void *b) {
    const struct space *x = a;
    const struct space *y = b;
    int cmp = strcmp(x->user, y->user);

    if (cmp != 0)
        return cmp;
    if (x->asid != y->asid)
        return x->asid < y->asid ? -1 : 1;
    return 0;
}

/* CPU counters arrive in 1024-microsecond units; print them as h:mm:ss. */
static void fmt_cpu(unsigned long long units, char *out, size_t outlen) {
    unsigned long long secs = (units * 1024ULL + 500000ULL) / 1000000ULL;

    snprintf(out, outlen, "%llu:%02llu:%02llu", secs / 3600ULL,
             secs % 3600ULL / 60ULL, secs % 60ULL);
}

static void print_rows(const struct space *sp, int n) {
    char line[MSG_LEN];
    char cpu[CPU_LEN];
    char asid[ASID_LEN];
    char pid[PID_LEN];
    int i;

    snprintf(line, sizeof(line),
             "%-8.8s %-6.6s %-4.4s %-8.8s %-10.10s %-8.8s %s\n", "USER", "TYPE",
             "ASID", "JOBNAME", "LOGONPROC", "PID", "CPU");
    emit_str(STDOUT_FILENO, line);
    for (i = 0; i < n; i++) {
        fmt_cpu(sp[i].cpu, cpu, sizeof(cpu));
        if (sp[i].asid != 0)
            snprintf(asid, sizeof(asid), "%04X", sp[i].asid);
        else
            copy_field(asid, sizeof(asid), "-");
        if (sp[i].pid > 0)
            snprintf(pid, sizeof(pid), "%" PRId32, sp[i].pid);
        else
            copy_field(pid, sizeof(pid), "-");
        snprintf(line, sizeof(line),
                 "%-8.8s %-6.6s %-4.4s %-8.8s %-10.10s %-8.8s %s\n", sp[i].user,
                 sp[i].type, asid, sp[i].job, sp[i].logon, pid, cpu);
        emit_str(STDOUT_FILENO, line);
    }
}

int main(int argc, char **argv) {
    static struct space spaces[MAX_SPACES];
    static struct space rows[MAX_SPACES];
    static struct proc procs[MAX_PROCS];
    int nsp;
    int npr;
    int arg;

    for (arg = 1; arg < argc; arg++) {
        if (strcmp(argv[arg], "-h") == 0 ||
            strcmp(argv[arg], "--help") == 0) {
            emit_str(STDOUT_FILENO, usage_text);
            return 0;
        } else if (strcmp(argv[arg], "-V") == 0 ||
                   strcmp(argv[arg], "--version") == 0) {
            emit_str(STDOUT_FILENO, "whoson " WHOSON_VERSION "\n");
            return 0;
        } else {
            usage_error("whoson: unknown argument: %s", argv[arg]);
            return 1;
        }
    }
    nsp = ascb_scan(spaces, MAX_SPACES);
    if (nsp == 0) {
        error_msg("whoson: no address spaces found\n");
        return 1;
    }
    npr = ps_scan(procs, MAX_PROCS);
    nsp = collect(rows, MAX_SPACES, spaces, nsp, procs, npr);
    qsort(rows, (size_t)nsp, sizeof(rows[0]), compare_rows);
    print_rows(rows, nsp);
    return 0;
}
