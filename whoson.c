/*
 * SPDX-License-Identifier: MIT OR Apache-2.0 OR BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Sean Kurtz
 *
 * whoson - who is logged on to this z/OS system, and how (TSO/E, z/OSMF, SSH).
 *
 * Sessions come from the address space control blocks, not from a subsystem
 * or a console command: CVT (PSA+X'10') -> ASVT -> every allocated ASCB. That
 * walk reads only common storage, which is mapped into every address space,
 * so it needs no APF authorization, no supervisor state and no key zero.
 * ASCBJBNI holds the job name; the logon procedure comes from the session's
 * CSCB (CHCLS), not from ASCBJBNS, which only names how the address space was
 * started. A non-zero ASCBTSB, or a CSCB track id of X'01', marks TSO/E.
 *
 * The CPU column is the address space's TCB time (ASCBEJST), SRB time
 * (ASCBSRBT) and preemptable SRB time (the ASSB's ASSBASST). All three are
 * TOD-clock accumulators: the low 12 bits are the fraction, so >> 12 is
 * microseconds.
 *
 * SSH sessions come from the process table: sshd forks one worker per
 * connection, so a process whose parent is an sshd (and which is not itself
 * an sshd) is a login. w_getpsent supplies that tree; ps supplies the
 * address-space id (BPXYPGPS has one, the C W_PSPROC does not). The join to
 * the ASCB is by ASID, not by job name: sshd's fork job names repeat.
 *
 * Control blocks are read through volatile pointers on purpose: nothing in
 * this file may be cached across the walk, because the blocks change under
 * it. Every read of a control block is guarded: an address space can end
 * between the ASVT read and the ASCB read, and a stale pointer must skip one
 * row, not abend the program.
 *
 * Character conversion follows the zopen conventions: source and output are
 * ISO8859-1 (CCSID 819) and the descriptor's tag says what the destination
 * expects. A text file with no tag (or a non-EBCDIC tag) is tagged 819 and
 * receives ASCII; an EBCDIC file, terminal or pipe is tagged IBM-1047 and
 * the C runtime converts the ASCII writes (fcntl F_CONTROL_CVT). Nothing is
 * decided from isatty(), and no byte is converted twice.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pwd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ps.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MSG_LEN 512
#define NAME_LEN 16
#define JOB_LEN 9
#define ASID_LEN 8
#define PID_LEN 12
#define CPU_LEN 16
#define PATH_LEN 64
#define OUT_LEN 8192
#define CMD_BUF 2048
#define TTY_BUF 2048

#ifdef PS_PATHBLEN
#define PS_PATH_BUF PS_PATHBLEN
#else
#define PS_PATH_BUF 1027
#endif

/* CVTASVT. The ASVT's own head, then ASVTENTY, the array of ASCB addresses;
 * a set high bit marks an ASID that is free or non-reusable. */
#define CVT_ASVT_OFF 0x22Cu
#define ASVT_ACRONYM_OFF 0x200u
#define ASVT_MAXU_OFF 0x204u
#define ASVT_ENTY_OFF 0x210u
#define ASVT_ACRONYM 0xC1E2E5E3u /* "ASVT" in EBCDIC */
#define ASCB_ACRONYM 0xC1E2C3C2u /* "ASCB" in EBCDIC */

/* Field offsets are checked against these overlays (static_assert below).
 * The overlays describe layout only; every read goes through the accessors,
 * so the compiler never assumes the block is stable. */
struct ascb {
    uint32_t ascbascb;
    unsigned char pad0[0x24 - 0x04];
    uint16_t ascbasid;
    unsigned char pad1[0x38 - 0x26];
    uint32_t ascbcscb;
    uint32_t ascbtsb;
    uint64_t ascbejst;
    unsigned char pad2[0xAC - 0x48];
    uint32_t ascbjbni;
    uint32_t ascbjbns;
    unsigned char pad3[0xC8 - 0xB4];
    uint64_t ascbsrbt;
    unsigned char pad4[0x150 - 0xD0];
    uint32_t ascbassb;
};

struct cscb {
    unsigned char pad0[0x08];
    char chkey[8];
    char chcls[8];
    unsigned char pad1[0x1C - 0x18];
    unsigned char chtrkid;
};

struct assb {
    unsigned char pad[0x160];
    uint64_t assbasst;
};

_Static_assert(offsetof(struct ascb, ascbasid) == 0x24, "ASCBASID");
_Static_assert(offsetof(struct ascb, ascbcscb) == 0x38, "ASCBCSCB");
_Static_assert(offsetof(struct ascb, ascbtsb) == 0x3C, "ASCBTSB");
_Static_assert(offsetof(struct ascb, ascbejst) == 0x40, "ASCBEJST");
_Static_assert(offsetof(struct ascb, ascbjbni) == 0xAC, "ASCBJBNI");
_Static_assert(offsetof(struct ascb, ascbjbns) == 0xB0, "ASCBJBNS");
_Static_assert(offsetof(struct ascb, ascbsrbt) == 0xC8, "ASCBSRBT");
_Static_assert(offsetof(struct ascb, ascbassb) == 0x150, "ASCBASSB");
_Static_assert(offsetof(struct cscb, chkey) == 0x08, "CHKEY");
_Static_assert(offsetof(struct cscb, chcls) == 0x10, "CHCLS");
_Static_assert(offsetof(struct cscb, chtrkid) == 0x1C, "CHTRKID");
_Static_assert(offsetof(struct assb, assbasst) == 0x160, "ASSBASST");

/* One address space, as read from its ASCB, indexed by ASID - 1. */
struct space {
    bool tso;
    uint64_t us; /* microseconds of CPU */
    char job[JOB_LEN];
    char user[NAME_LEN];
    char logon[JOB_LEN];
};

/* One output row. TSO rows are built from a space; SSH rows from a process. */
struct row {
    unsigned asid;
    bool tso;
    uint64_t us;
    pid_t pid;
    const char *type;
    char job[JOB_LEN];
    char logon[JOB_LEN];
    char user[NAME_LEN];
};

struct proc {
    pid_t pid;
    pid_t ppid;
    uid_t euid;
    char name[PATH_LEN];
};

struct pid_asid {
    pid_t pid;
    unsigned asid;
};

static const char usage_text[] =
    "usage: whoson [--help | --version]\n"
    "\n"
    "List the interactive sessions on this system: TSO/E, z/OSMF (a TSO/E\n"
    "session started by the z/OSMF logon procedure) and SSH. Started tasks,\n"
    "OMVS daemons and the processes inside a session are not listed.\n"
    "\n"
    "Options:\n"
    "  -h, --help     show this help and exit\n"
    "  -V, --version  show the version and exit\n"
    "\n"
    "z/OSMF is recognised by its logon procedure name. Set\n"
    "WHOSON_ZOSMF_PROCS to a comma-separated list to match other names\n"
    "(default: IZUFPROC).\n";

/* Set by the Makefile from its VERSION, so the binary and the tag agree. */
#ifndef WHOSON_VERSION
#define WHOSON_VERSION "unknown"
#endif

/* A control-block read that faults is caught here and turned into a skipped
 * row. */
static sigjmp_buf fault_env;
static volatile sig_atomic_t fault_armed;

static void on_fault(int sig) {
    if (fault_armed)
        siglongjmp(fault_env, 1);
    _exit(128 + sig);
}

static void guard(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
#ifdef SIGBUS
    sigaction(SIGBUS, &sa, NULL);
#endif
}

static void unguard(void) {
    signal(SIGSEGV, SIG_DFL);
#ifdef SIGBUS
    signal(SIGBUS, SIG_DFL);
#endif
}

static uint8_t read_u8(uint32_t addr) {
    return *(const volatile uint8_t *)(uintptr_t)addr;
}

static uint16_t read_u16(uint32_t addr) {
    return *(const volatile uint16_t *)(uintptr_t)addr;
}

static uint32_t read_u32(uint32_t addr) {
    return *(const volatile uint32_t *)(uintptr_t)addr;
}

static uint64_t read_u64(uint32_t addr) {
    return *(const volatile uint64_t *)(uintptr_t)addr;
}

static void write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        buf += n;
        len -= (size_t)n;
    }
}

/* Tag fd with ccsid as text. zopen's setccsid(): an untagged file created by
 * the shell becomes ISO8859-1 rather than an anonymous EBCDIC file. */
static int tag_fd(int fd, unsigned ccsid) {
    attrib_t a;

    memset(&a, 0, sizeof(a));
    a.att_filetagchg = 1;
    a.att_filetag.ft_ccsid = (unsigned short)ccsid;
    a.att_filetag.ft_txtflag = 1;
    return __fchattr(fd, &a, sizeof(a));
}

static int set_cvt(int fd, int cmd, int pccsid, int fccsid) {
    struct f_cnvrt c;

    c.cvtcmd = cmd;
    c.pccsid = (short)pccsid;
    c.fccsid = (short)fccsid;
    return fcntl(fd, F_CONTROL_CVT, &c);
}

/* The conversion setting belongs to the open file description, which the
 * calling shell keeps after whoson exits. Save it and put it back, or the
 * shell's own output would inherit ours. */
struct cvt_saved {
    int fd;
    struct f_cnvrt v;
};

static struct cvt_saved saved_cvt[2];
static size_t nsaved;

static void restore_cvt(void) {
    size_t i;

    for (i = 0; i < nsaved; i++)
        (void)fcntl(saved_cvt[i].fd, F_CONTROL_CVT, &saved_cvt[i].v);
}

static void save_cvt(int fd) {
    struct f_cnvrt q;

    memset(&q, 0, sizeof(q));
    q.cvtcmd = QUERYCVT;
    if (fcntl(fd, F_CONTROL_CVT, &q) != 0)
        return;
    saved_cvt[nsaved].fd = fd;
    saved_cvt[nsaved].v = q;
    nsaved++;
}

static bool ebcdic_tag(const struct stat *st) {
    return st->st_tag.ft_txtflag &&
           (st->st_tag.ft_ccsid == 37 || st->st_tag.ft_ccsid == 1047);
}

/* Write buf in the character set fd expects. Files are the ASCII half of the
 * zopen rule; terminals, pipes and EBCDIC-tagged files are IBM-1047. Only a
 * file's tag is changed (an untagged file becomes ISO8859-1); a pipe or pty
 * keeps its tag and just gets the conversion, so the runtime's own output to
 * the same descriptor is not relabelled. */
static void emit(int fd, char *buf, size_t len) {
    struct stat st;
    bool ascii = false;
    int fccsid = 1047;

    if (fstat(fd, &st) == 0) {
        if (S_ISREG(st.st_mode)) {
            ascii = !ebcdic_tag(&st);
            if (ascii && (!st.st_tag.ft_txtflag || st.st_tag.ft_ccsid == 0))
                (void)tag_fd(fd, 819);
        } else if (S_ISSOCK(st.st_mode)) {
            ascii = true;
        } else if (st.st_tag.ft_txtflag && st.st_tag.ft_ccsid != 0 &&
                   !ebcdic_tag(&st)) {
            ascii = true; /* a pipe or pty that asked for ASCII */
        } else if (ebcdic_tag(&st)) {
            fccsid = (int)st.st_tag.ft_ccsid;
        }
    }
    if (ascii) {
        (void)set_cvt(fd, SETCVTOFF, 0, 0);
        write_all(fd, buf, len);
        return;
    }
    if (set_cvt(fd, SETCVTON, 819, fccsid) == 0) {
        write_all(fd, buf, len);
        return;
    }
    /* No fcntl support (fstat failed): convert by hand. */
    (void)set_cvt(fd, SETCVTOFF, 0, 0);
    __a2e_l(buf, len);
    write_all(fd, buf, len);
}

static void emit_str(int fd, const char *text) {
    char buf[MSG_LEN];
    size_t len = strlen(text);

    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    emit(fd, buf, len);
}

static void vmessage(bool usage, const char *fmt, va_list ap) {
    char text[MSG_LEN];

    vsnprintf(text, sizeof(text), fmt, ap);
    emit_str(STDERR_FILENO, text);
    if (usage) {
        emit_str(STDERR_FILENO, "\n");
        emit_str(STDERR_FILENO, usage_text);
    }
}

static void error_msg(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vmessage(false, fmt, ap);
    va_end(ap);
}

static void usage_error(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vmessage(true, fmt, ap);
    va_end(ap);
}

/* Copy the 8-byte EBCDIC name a control block points at. These pointers
 * address common storage, so no cross-memory services are needed. */
static void read_name(uint32_t ptr, char *dst, size_t dstlen) {
    unsigned char raw[8];
    size_t i;
    size_t n = 0;

    if (dstlen == 0)
        return;
    dst[0] = '\0';
    if (ptr == 0)
        return;
    for (i = 0; i < sizeof(raw); i++)
        raw[i] = read_u8(ptr + (uint32_t)i);
    __e2a_l((char *)raw, sizeof(raw));
    for (i = 0; i < sizeof(raw) && n + 1 < dstlen; i++) {
        if (raw[i] == ' ' || raw[i] == '\0')
            break;
        if (raw[i] < 0x20 || raw[i] > 0x7E)
            break;
        dst[n++] = (char)raw[i];
    }
    dst[n] = '\0';
}

static void read_tagged_name(uint32_t addr, char *dst, size_t dstlen) {
    unsigned char raw[8];
    size_t i;
    size_t n = 0;

    if (dstlen == 0)
        return;
    dst[0] = '\0';
    for (i = 0; i < sizeof(raw); i++)
        raw[i] = read_u8(addr + (uint32_t)i);
    __e2a_l((char *)raw, sizeof(raw));
    for (i = 0; i < sizeof(raw) && n + 1 < dstlen; i++) {
        if (raw[i] == ' ' || raw[i] == '\0')
            break;
        if (raw[i] < 0x20 || raw[i] > 0x7E)
            break;
        dst[n++] = (char)raw[i];
    }
    dst[n] = '\0';
}

/* Read one ASCB. Returns 0 and fills *out on success; -1 skips it. */
static int scan_one(unsigned idx, uint32_t entry, uint32_t entries,
                    struct space *out) {
    struct space tmp;
    uint32_t ascb = entry & 0x7FFFFFFFu;
    uint16_t asid;
    uint32_t cscb;
    uint64_t tod;
    bool tso;
    char key[NAME_LEN];
    char cls[JOB_LEN];

    memset(&tmp, 0, sizeof(tmp));
    memset(key, 0, sizeof(key));
    memset(cls, 0, sizeof(cls));
    fault_armed = 1;
    if (sigsetjmp(fault_env, 1) != 0) {
        fault_armed = 0;
        return -1;
    }
    if (read_u32(ascb) != ASCB_ACRONYM)
        goto fail;
    asid = read_u16(ascb + (uint32_t)offsetof(struct ascb, ascbasid));
    if (asid != idx + 1)
        goto fail;
    tod = read_u64(ascb + (uint32_t)offsetof(struct ascb, ascbejst)) +
          read_u64(ascb + (uint32_t)offsetof(struct ascb, ascbsrbt));
    cscb = read_u32(ascb + (uint32_t)offsetof(struct ascb, ascbcscb));
    tso = read_u32(ascb + (uint32_t)offsetof(struct ascb, ascbtsb)) != 0;
    {
        uint32_t assb =
            read_u32(ascb + (uint32_t)offsetof(struct ascb, ascbassb));

        if (assb != 0)
            tod += read_u64(assb + (uint32_t)offsetof(struct assb, assbasst));
    }
    if (cscb != 0) {
        read_tagged_name(cscb + (uint32_t)offsetof(struct cscb, chkey), key,
                         sizeof(key));
        read_tagged_name(cscb + (uint32_t)offsetof(struct cscb, chcls), cls,
                         sizeof(cls));
        if (read_u8(cscb + (uint32_t)offsetof(struct cscb, chtrkid)) == 0x01)
            tso = true;
    }
    read_name(read_u32(ascb + (uint32_t)offsetof(struct ascb, ascbjbni)),
              tmp.job, sizeof(tmp.job));
    read_name(read_u32(ascb + (uint32_t)offsetof(struct ascb, ascbjbns)),
              tmp.logon, sizeof(tmp.logon));
    /* The ASVT entry must still name the same ASCB: if the address space
     * ended while we were reading it, the block may already be reused. */
    if (read_u32(entries + 4u * idx) != entry)
        goto fail;
    tmp.tso = tso;
    tmp.us = tod >> 12;
    if (tmp.job[0] == '\0')
        snprintf(tmp.job, sizeof(tmp.job), "%s", tmp.logon);
    if (key[0] != '\0')
        snprintf(tmp.user, sizeof(tmp.user), "%s", key);
    else
        snprintf(tmp.user, sizeof(tmp.user), "%s", tmp.job);
    if (cls[0] != '\0')
        snprintf(tmp.logon, sizeof(tmp.logon), "%s", cls);
    else
        tmp.logon[0] = '\0';
    *out = tmp;
    fault_armed = 0;
    return 0;
fail:
    fault_armed = 0;
    return -1;
}

static int read_asvt(unsigned *maxu, uint32_t *entries) {
    uint32_t cvt;
    uint32_t asvt;

    fault_armed = 1;
    if (sigsetjmp(fault_env, 1) != 0) {
        fault_armed = 0;
        return -1;
    }
    cvt = read_u32(16);
    if (cvt == 0)
        goto fail;
    asvt = read_u32(cvt + CVT_ASVT_OFF);
    if (asvt == 0)
        goto fail;
    if (read_u32(asvt + ASVT_ACRONYM_OFF) != ASVT_ACRONYM)
        goto fail;
    *maxu = read_u32(asvt + ASVT_MAXU_OFF) & 0xFFFFu;
    if (*maxu == 0)
        goto fail;
    *entries = asvt + ASVT_ENTY_OFF;
    fault_armed = 0;
    return 0;
fail:
    fault_armed = 0;
    return -1;
}

static int ascb_scan(uint32_t entries, unsigned maxu, struct space *snap,
                     unsigned *found) {
    unsigned i;

    *found = 0;
    for (i = 0; i < maxu; i++) {
        uint32_t entry = read_u32(entries + 4u * i);

        if ((entry & 0x80000000u) != 0 || (entry & 0x7FFFFFFFu) == 0)
            continue;
        if (scan_one(i, entry, entries, &snap[i]) == 0)
            (*found)++;
    }
    return 0;
}

static FILE *ps_open(void) {
    return popen("/bin/ps -A -o pid,xasid", "r");
}

static int read_all(int fd, char **out, size_t *outlen) {
    size_t cap = 16384;
    size_t len = 0;
    char *buf = malloc(cap + 1);

    if (buf == NULL)
        return -1;
    for (;;) {
        ssize_t n;

        if (len == cap) {
            char *grown;

            if (cap >= (8u << 20)) {
                free(buf);
                errno = EFBIG;
                return -1;
            }
            grown = realloc(buf, cap * 2 + 1);
            if (grown == NULL) {
                free(buf);
                return -1;
            }
            buf = grown;
            cap *= 2;
        }
        n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return -1;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    *out = buf;
    *outlen = len;
    return 0;
}

/* ps on a pipe is ASCII when the pipe is tagged ASCII and EBCDIC when it is
 * not; the content decides, not the environment. A line ends at LF, NL or
 * the EBCDIC 0x25/0x15 code points. */
static void text_to_ascii(char *buf, size_t len) {
    size_t i;

    for (i = 0; i < len; i++) {
        if ((unsigned char)buf[i] >= 0x80) {
            __e2a_l(buf, len);
            return;
        }
    }
}

/* Tell read() what the pipe carries, using its tag: 1 = valid ASCII, 2 =
 * the runtime was told to convert (read returns ASCII), 0 = untagged, so the
 * bytes must be sniffed after the read. */
static int read_setup(int fd) {
    struct stat st;

    if (fstat(fd, &st) == 0 && st.st_tag.ft_txtflag) {
        unsigned ccsid = st.st_tag.ft_ccsid;

        if (ccsid == 37 || ccsid == 1047) {
            if (set_cvt(fd, SETCVTON, 819, (int)ccsid) == 0)
                return 2;
        } else if (ccsid != 0) {
            (void)set_cvt(fd, SETCVTOFF, 0, 0);
            return 1;
        }
    }
    (void)set_cvt(fd, SETCVTOFF, 0, 0);
    return 0;
}

static int add_pid_asid(struct pid_asid **map, size_t *n, size_t *cap,
                        pid_t pid, unsigned asid) {
    if (*n == *cap) {
        size_t ncap = *cap ? *cap * 2 : 64;
        struct pid_asid *grown = realloc(*map, ncap * sizeof(**map));

        if (grown == NULL)
            return -1;
        *map = grown;
        *cap = ncap;
    }
    (*map)[*n].pid = pid;
    (*map)[*n].asid = asid;
    (*n)++;
    return 0;
}

static bool is_header(const char *s) {
    return !isdigit((unsigned char)s[0]);
}

static int ps_scan(struct pid_asid **map, size_t *count) {
    FILE *fp;
    char *buf = NULL;
    size_t len = 0;
    size_t n = 0;
    size_t cap = 0;
    size_t i;
    size_t start = 0;
    int status;
    int mode;

    *map = NULL;
    *count = 0;
    fp = ps_open();
    if (fp == NULL) {
        error_msg("whoson: cannot run /bin/ps: %s\n", strerror(errno));
        return -1;
    }
    mode = read_setup(fileno(fp));
    if (read_all(fileno(fp), &buf, &len) != 0) {
        error_msg("whoson: cannot read ps output: %s\n", strerror(errno));
        pclose(fp);
        return -1;
    }
    status = pclose(fp);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error_msg("whoson: ps failed\n");
        free(buf);
        return -1;
    }
    if (mode == 0)
        text_to_ascii(buf, len);
    for (i = 0; i <= len; i++) {
        char c = i < len ? buf[i] : '\n';

        if (c != '\n' && c != 0x15 && c != 0x25)
            continue;
        buf[i < len ? i : len] = '\0';
        if (buf[start] != '\0') {
            int pid;
            unsigned asid;

            if (sscanf(&buf[start], "%d %x", &pid, &asid) == 2) {
                if (add_pid_asid(map, &n, &cap, (pid_t)pid, asid) != 0) {
                    error_msg("whoson: out of memory\n");
                    free(buf);
                    return -1;
                }
            } else if (!is_header(&buf[start])) {
                error_msg("whoson: cannot parse ps line: %s\n", &buf[start]);
                free(*map);
                *map = NULL;
                free(buf);
                return -1;
            }
        }
        start = i + 1;
    }
    free(buf);
    if (n == 0) {
        error_msg("whoson: ps returned no processes\n");
        free(*map);
        *map = NULL;
        return -1;
    }
    *count = n;
    return 0;
}

static int cmp_pid_asid(const void *a, const void *b) {
    const struct pid_asid *x = a;
    const struct pid_asid *y = b;

    if (x->pid != y->pid)
        return x->pid < y->pid ? -1 : 1;
    return 0;
}

static const struct pid_asid *find_asid(const struct pid_asid *map, size_t n,
                                        pid_t pid) {
    struct pid_asid key;

    key.pid = pid;
    key.asid = 0;
    return bsearch(&key, map, n, sizeof(*map), cmp_pid_asid);
}

static int cmp_proc(const void *a, const void *b) {
    const struct proc *x = a;
    const struct proc *y = b;

    if (x->pid != y->pid)
        return x->pid < y->pid ? -1 : 1;
    return 0;
}

static const struct proc *find_proc(const struct proc *procs, size_t n,
                                    pid_t pid) {
    struct proc key;

    memset(&key, 0, sizeof(key));
    key.pid = pid;
    return bsearch(&key, procs, n, sizeof(*procs), cmp_proc);
}

static bool is_sshd(const char *path) {
    const char *base = strrchr(path, '/');

    base = base != NULL ? base + 1 : path;
    return strcmp(base, "sshd") == 0 || strcmp(base, "sshd-session") == 0;
}

static int append_proc(struct proc **procs, size_t *n, size_t *cap,
                       const W_PSPROC *ps, const char *path) {
    const char *base;
    size_t len;

    if (*n == *cap) {
        size_t ncap = *cap ? *cap * 2 : 64;
        struct proc *grown = realloc(*procs, ncap * sizeof(**procs));

        if (grown == NULL)
            return -1;
        *procs = grown;
        *cap = ncap;
    }
    base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    len = strlen(base);
    if (len >= sizeof((*procs)[*n].name))
        len = sizeof((*procs)[*n].name) - 1;
    memcpy((*procs)[*n].name, base, len);
    (*procs)[*n].name[len] = '\0';
    (*procs)[*n].pid = ps->ps_pid;
    (*procs)[*n].ppid = ps->ps_ppid;
    (*procs)[*n].euid = ps->ps_euid;
    (*n)++;
    return 0;
}

static int proc_scan(struct proc **procs, size_t *count) {
    W_PSPROC ps;
    char *path;
    char *cmd;
    char *tty;
    size_t n = 0;
    size_t cap = 0;
    int token = 0;

    *procs = NULL;
    *count = 0;
    path = malloc(PS_PATH_BUF);
    cmd = malloc(CMD_BUF);
    tty = malloc(TTY_BUF);
    if (path == NULL || cmd == NULL || tty == NULL) {
        error_msg("whoson: out of memory\n");
        free(path);
        free(cmd);
        free(tty);
        return -1;
    }
    for (;;) {
        int next;

        memset(&ps, 0, sizeof(ps));
        ps.ps_pathlen = PS_PATH_BUF;
        ps.ps_pathptr = path;
        ps.ps_cmdlen = CMD_BUF;
        ps.ps_cmdptr = cmd;
        ps.ps_conttylen = TTY_BUF;
        ps.ps_conttyptr = tty;
        next = w_getpsent(token, &ps, sizeof(ps));
        if (next < 0) {
            error_msg("whoson: cannot read the process table: %s\n",
                      strerror(errno));
            free(*procs);
            *procs = NULL;
            free(path);
            free(cmd);
            free(tty);
            return -1;
        }
        if (next == 0)
            break;
        token = next;
        path[PS_PATH_BUF - 1] = '\0';
        if (append_proc(procs, &n, &cap, &ps, path) != 0) {
            error_msg("whoson: out of memory\n");
            free(*procs);
            *procs = NULL;
            free(path);
            free(cmd);
            free(tty);
            return -1;
        }
    }
    free(path);
    free(cmd);
    free(tty);
    if (n == 0) {
        error_msg("whoson: the process table is empty\n");
        free(*procs);
        *procs = NULL;
        return -1;
    }
    qsort(*procs, n, sizeof(**procs), cmp_proc);
    *count = n;
    return 0;
}

/* A logon procedure name from WHOSON_ZOSMF_PROCS; z/OSMF passes the procedure
 * it was started with, so the name is the only clue common storage has. */
static bool logon_is_zosmf(const char *logon) {
    const char *list = getenv("WHOSON_ZOSMF_PROCS");
    const char *p;

    if (logon == NULL || logon[0] == '\0')
        return false;
    p = list != NULL && list[0] != '\0' ? list : "IZUFPROC";
    for (;;) {
        const char *comma = strchr(p, ',');
        const char *end = comma != NULL ? comma : p + strlen(p);
        const char *s = p;
        size_t len;

        while (s < end && (*s == ' ' || *s == '\t'))
            s++;
        while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
            end--;
        len = (size_t)(end - s);
        if (len != 0 && len == strlen(logon) && strncmp(s, logon, len) == 0)
            return true;
        if (comma == NULL)
            break;
        p = comma + 1;
    }
    return false;
}

static void user_name(uid_t uid, char *out, size_t outlen) {
    static struct {
        uid_t uid;
        char name[NAME_LEN];
    } cache[16];
    static size_t used;
    size_t i;
    struct passwd *pw;

    for (i = 0; i < used; i++) {
        if (cache[i].uid == uid) {
            snprintf(out, outlen, "%s", cache[i].name);
            return;
        }
    }
    pw = getpwuid(uid);
    if (pw != NULL)
        snprintf(cache[used].name, sizeof(cache[used].name), "%s", pw->pw_name);
    else
        snprintf(cache[used].name, sizeof(cache[used].name), "%u",
                 (unsigned)uid);
    cache[used].uid = uid;
    snprintf(out, outlen, "%s", cache[used].name);
    if (used + 1 < sizeof(cache) / sizeof(cache[0]))
        used++;
}

static int cmp_rows(const void *a, const void *b) {
    const struct row *x = a;
    const struct row *y = b;
    int cmp = strcmp(x->user, y->user);

    if (cmp != 0)
        return cmp;
    if (x->asid != y->asid)
        return x->asid < y->asid ? -1 : 1;
    if (x->pid != y->pid)
        return x->pid < y->pid ? -1 : 1;
    return 0;
}

/* TOD units are 2^-12 microseconds; print the total as h:mm:ss. */
static void fmt_cpu(uint64_t us, char *out, size_t outlen) {
    uint64_t secs = (us + 500000u) / 1000000u;

    snprintf(out, outlen, "%" PRIu64 ":%02" PRIu64 ":%02" PRIu64, secs / 3600u,
             secs % 3600u / 60u, secs % 60u);
}

struct outbuf {
    char buf[OUT_LEN];
    size_t len;
};

static void out_flush(struct outbuf *o) {
    if (o->len != 0)
        emit(STDOUT_FILENO, o->buf, o->len);
    o->len = 0;
}

static void out_add(struct outbuf *o, const char *fmt, ...) {
    char line[MSG_LEN];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;
    if (o->len + (size_t)n > sizeof(o->buf))
        out_flush(o);
    memcpy(o->buf + o->len, line, (size_t)n);
    o->len += (size_t)n;
}

static void print_rows(struct row *rows, size_t n) {
    struct outbuf out;
    char cpu[CPU_LEN];
    char asid[ASID_LEN];
    char pid[PID_LEN];
    size_t i;

    out.len = 0;
    out_add(&out, "%-8.8s %-6.6s %-4.4s %-8.8s %-10.10s %-8.8s %s\n", "USER",
            "TYPE", "ASID", "JOBNAME", "LOGONPROC", "PID", "CPU");
    for (i = 0; i < n; i++) {
        fmt_cpu(rows[i].us, cpu, sizeof(cpu));
        if (rows[i].asid != 0)
            snprintf(asid, sizeof(asid), "%04X", rows[i].asid);
        else
            snprintf(asid, sizeof(asid), "%s", "-");
        if (rows[i].pid > 0)
            snprintf(pid, sizeof(pid), "%" PRId32, rows[i].pid);
        else
            snprintf(pid, sizeof(pid), "%s", "-");
        out_add(&out, "%-8.8s %-6.6s %-4.4s %-8.8s %-10.10s %-8.8s %s\n",
                rows[i].user, rows[i].type, asid, rows[i].job, rows[i].logon,
                pid, cpu);
    }
    out_flush(&out);
}

static int collect(struct row *rows, size_t maxrows, const struct space *snap,
                   unsigned maxu, const struct proc *procs, size_t nprocs,
                   const struct pid_asid *map, size_t nmap) {
    size_t n = 0;
    unsigned i;
    size_t k;

    for (i = 0; i < maxu && n < maxrows; i++) {
        if (!snap[i].tso)
            continue;
        rows[n].asid = i + 1;
        rows[n].tso = true;
        rows[n].us = snap[i].us;
        rows[n].pid = 0;
        rows[n].type = logon_is_zosmf(snap[i].logon) ? "z/OSMF" : "TSO/E";
        snprintf(rows[n].user, sizeof(rows[n].user), "%s",
                 snap[i].user[0] != '\0' ? snap[i].user : snap[i].job);
        snprintf(rows[n].job, sizeof(rows[n].job), "%s", snap[i].job);
        snprintf(rows[n].logon, sizeof(rows[n].logon), "%s",
                 snap[i].logon[0] != '\0' ? snap[i].logon : "-");
        n++;
    }
    for (k = 0; k < nprocs && n < maxrows; k++) {
        const struct proc *parent;
        const struct pid_asid *pa;
        unsigned asid = 0;
        uint64_t us = 0;
        const char *job = "-";

        parent = find_proc(procs, nprocs, procs[k].ppid);
        if (parent == NULL || !is_sshd(parent->name) || is_sshd(procs[k].name))
            continue;
        pa = find_asid(map, nmap, procs[k].pid);
        if (pa != NULL && pa->asid >= 1 && pa->asid <= maxu) {
            asid = pa->asid;
            us = snap[asid - 1].us;
            if (snap[asid - 1].job[0] != '\0')
                job = snap[asid - 1].job;
        }
        rows[n].asid = asid;
        rows[n].tso = false;
        rows[n].us = us;
        rows[n].pid = procs[k].pid;
        rows[n].type = "SSH";
        snprintf(rows[n].job, sizeof(rows[n].job), "%s", job);
        snprintf(rows[n].logon, sizeof(rows[n].logon), "%s", "-");
        user_name(procs[k].euid, rows[n].user, sizeof(rows[n].user));
        n++;
    }
    return (int)n;
}

int main(int argc, char **argv) {
    struct space *snap;
    struct row *rows;
    struct proc *procs;
    struct pid_asid *map;
    size_t nmap;
    size_t nprocs;
    unsigned maxu;
    unsigned found;
    uint32_t entries;
    int nrows;
    int arg;
    size_t space_bytes;

    save_cvt(STDOUT_FILENO);
    save_cvt(STDERR_FILENO);
    (void)atexit(restore_cvt);
    for (arg = 1; arg < argc; arg++) {
        if (strcmp(argv[arg], "-h") == 0 || strcmp(argv[arg], "--help") == 0) {
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
    guard();
    if (read_asvt(&maxu, &entries) != 0) {
        unguard();
        error_msg("whoson: cannot read the ASVT\n");
        return 1;
    }
    space_bytes = (sizeof(struct space) + 15u) & ~(size_t)15u;
    snap = calloc(1, (size_t)maxu * space_bytes);
    if (snap == NULL) {
        unguard();
        error_msg("whoson: out of memory for %u address spaces\n", maxu);
        return 1;
    }
    ascb_scan(entries, maxu, snap, &found);
    unguard();
    if (found == 0) {
        free(snap);
        error_msg("whoson: no address spaces found\n");
        return 1;
    }
    if (ps_scan(&map, &nmap) != 0) {
        free(snap);
        return 1;
    }
    if (proc_scan(&procs, &nprocs) != 0) {
        free(map);
        free(snap);
        return 1;
    }
    qsort(map, nmap, sizeof(*map), cmp_pid_asid);
    /* At most one TSO row per address space and one SSH row per process, so
     * the table can hold both lists; maxu alone would drop SSH sessions on a
     * system that shares address spaces (_BPX_SHAREAS). */
    rows = calloc((size_t)maxu + nprocs, sizeof(*rows));
    if (rows == NULL) {
        error_msg("whoson: out of memory for the session table\n");
        free(procs);
        free(map);
        free(snap);
        return 1;
    }
    nrows = collect(rows, (size_t)maxu + nprocs, snap, maxu, procs, nprocs, map,
                    nmap);
    qsort(rows, (size_t)nrows, sizeof(*rows), cmp_rows);
    print_rows(rows, (size_t)nrows);
    free(rows);
    free(procs);
    free(map);
    free(snap);
    return 0;
}
