/*
 * SPDX-License-Identifier: MIT OR Apache-2.0 OR BSD-2-Clause
 * SPDX-FileCopyrightText: 2026 Sean Kurtz
 *
 * whoson - who is logged on to this z/OS system, and how (TSO/E or SSH).
 *
 * Session data comes from ZOAU's opercmd through the shared library vector
 * (no shelling out). TSO/E users come from DISPLAY TS,ALL, the only display
 * that lists them here: DISPLAY TSO,... is not a valid command on this
 * system. SSH users come from DISPLAY OMVS,A=ALL, where each session is an
 * "sshd" process; sshd runs its listener and the pre-authentication half of
 * every session under a daemon account, so that account is dropped.
 *
 * The vector's opercmd splits its argument string into argv without a shell,
 * so the console command has to arrive as one argv element: it is passed in
 * double quotes, "D TS,A". Unquoted, the D is taken for an option letter and
 * the call fails with opercmd's usage text (rc 2). The arguments stay in
 * EBCDIC; an ASCII-converted copy fails with rc 1 and no message.
 *
 * A TSO/E session reports its own CPU time (CT), elapsed time (ET), job id
 * (WUID/SCL/P) and region (RGP), so those columns need no second source. An
 * SSH session reports a start time instead, and its elapsed time is computed
 * from that.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <zoautil.h>

#define MAX_TOKENS 16
#define MAX_ARGS 64
#define MAX_PROCS 8192
#define MAX_ROWS 256
#define NAME_LEN 16
#define ASID_LEN 8
#define TIME_LEN 16
#define WUID_LEN 12
#define CPU_LEN 12
#define SCL_LEN 8
#define PRIO_LEN 4
#define RGP_LEN 12
#define PID_LEN 12
#define STATUS_LEN 12
#define CMD_LEN 256

struct proc {
	char user[NAME_LEN];
	char asid[ASID_LEN];
	char start[TIME_LEN];
	long pid;
	int ppid;
	char cmd[CMD_LEN];
};

struct row {
	char user[NAME_LEN];
	char session[8];
	char status[STATUS_LEN];
	char asid[ASID_LEN];
	char wuid[WUID_LEN];
	char cpu[CPU_LEN];
	char elapsed[TIME_LEN];
	char scl[SCL_LEN];
	char prio[PRIO_LEN];
	char rgp[RGP_LEN];
	char pid[PID_LEN];
};

static int usage_error(const char *msg)
{
	fprintf(stderr, "whoson: %s\n", msg);
	fprintf(stderr, "usage: whoson\n\n"
			"List logged-on users with session type (TSO/E, SSH).\n");
	return 2;
}

/* Split a display line in place; returns the token count. */
static int tokenize(char *line, char **tok)
{
	int n = 0;
	char *p = line;

	while (*p != '\0' && n < MAX_TOKENS) {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (*p == '\0')
			break;
		tok[n++] = p;
		while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' &&
		       *p != '\r')
			p++;
		if (*p != '\0')
			*p++ = '\0';
	}
	return n;
}

/* "hh.mm.ss", the form opercmd prints start times in. */
static int is_hms(const char *s)
{
	int digits = 0;
	int dots = 0;

	for (; *s != '\0'; s++) {
		if (*s >= '0' && *s <= '9')
			digits++;
		else if (*s == '.')
			dots++;
		else
			return 0;
	}
	return digits == 6 && dots == 2;
}

static long hms_to_secs(const char *s)
{
	long h = 0, m = 0, sec = 0;

	if (sscanf(s, "%ld.%ld.%ld", &h, &m, &sec) != 3)
		return -1;
	return h * 3600 + m * 60 + sec;
}

static void format_elapsed(const char *start, char *out, size_t outlen)
{
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	long then, delta;

	then = hms_to_secs(start);
	if (then < 0 || tm == NULL) {
		snprintf(out, outlen, "-");
		return;
	}
	delta = (long)tm->tm_hour * 3600 + (long)tm->tm_min * 60 + tm->tm_sec -
		then;
	if (delta < 0)
		delta += 86400;
	snprintf(out, outlen, "%02ld.%02ld.%02ld", delta / 3600,
		 delta % 3600 / 60, delta % 60);
}

static void copy_field(char *dst, size_t dstlen, const char *src)
{
	snprintf(dst, dstlen, "%s", src);
}

/* Parse DISPLAY OMVS,A=ALL: one 8-field row per process, each followed by an
 * indented detail line naming the command. */
static int parse_omvs(char *text, struct proc *procs, int max)
{
	int n = 0;
	char *line = text;
	char *tok[MAX_TOKENS];
	char raw[512];
	char work[512];

	while (line != NULL && n < max) {
		char *next = strchr(line, '\n');
		int count;

		if (next != NULL)
			*next++ = '\0';
		snprintf(raw, sizeof(raw), "%s", line);
		snprintf(work, sizeof(work), "%s", line);
		count = tokenize(work, tok);
		if (count == 8 && is_hms(tok[6]) && strchr(tok[5], '-') != NULL) {
			copy_field(procs[n].user, NAME_LEN, tok[0]);
			copy_field(procs[n].asid, ASID_LEN, tok[2]);
			copy_field(procs[n].start, TIME_LEN, tok[6]);
			procs[n].pid = strtol(tok[3], NULL, 10);
			procs[n].ppid = (int)strtol(tok[4], NULL, 10);
			procs[n].cmd[0] = '\0';
			n++;
		} else if (n > 0) {
			char *cmd = strstr(raw, "CMD=");

			if (cmd != NULL)
				copy_field(procs[n - 1].cmd, CMD_LEN, cmd + 4);
		}
		line = next;
	}
	return n;
}

/* The account that owns an sshd process whose parent is the init process owns
 * the listener, and with it the pre-authentication half of every session. */
static int is_daemon(const struct proc *procs, int n, const char *user)
{
	for (int i = 0; i < n; i++)
		if (procs[i].ppid == 1 && strstr(procs[i].cmd, "sshd") != NULL &&
		    strcmp(procs[i].user, user) == 0)
			return 1;
	return 0;
}

/* Parse DISPLAY TS,A: one record per TSO/E user, five lines long --
 *
 *   SKURTZ   OWT     A=0058   PER=NO   SMC=000  PGN=N/A  DMN=N/A  AFF=NONE
 *                   CT=000.058S  ET=01.22.42
 *                   WUID=TSU08375
 *                   WKL=TSO      SCL=TSO      P=1
 *                   RGP=N/A      SRVR=NO  QSC=NO
 *
 * A record starts at the line that carries A=; every field after that belongs
 * to the record above it.
 */
/* Elapsed and CPU times arrive in two shapes: "01.22.42" is hours.minutes
 * .seconds, and "019.705S" is seconds.milliseconds with an S suffix, which is
 * what a session younger than an hour reports. Normalise the latter. */
static void fmt_clock(const char *in, char *out, size_t outlen)
{
	size_t len = strlen(in);

	if (len == 0 || in[len - 1] != 'S') {
		snprintf(out, outlen, "%s", in);
		return;
	}
	{
		double secs = atof(in);
		long whole = (long)secs;

		snprintf(out, outlen, "%02ld.%02ld.%02ld", whole / 3600,
			 whole % 3600 / 60, whole % 60);
	}
}

/* Parse DISPLAY TS,A: one record per TSO/E user, five lines long --
 *
 *   SKURTZ   IN   O  A=0058   PER=NO   SMC=000  PGN=N/A  DMN=N/A  AFF=NONE
 *                   CT=000.458S  ET=019.705S
 *                   WUID=TSU08400
 *                   WKL=TSO      SCL=TSO      P=1
 *                   RGP=N/A      SRVR=NO  QSC=NO
 *
 * The number of words before A= varies ("OWT", "IN O", "WTOR S"), so the
 * record is found by looking for the A= token, and everything between the
 * user id and it is the status. Every field after that belongs to the record
 * above it.
 */
static int parse_ts(char *text, struct row *rows, int max)
{
	int n = 0;
	char *line = text;
	char *tok[MAX_TOKENS];
	char work[512];

	while (line != NULL && n < max) {
		char *next = strchr(line, '\n');
		int count;
		int aat = -1;

		if (next != NULL)
			*next++ = '\0';
		snprintf(work, sizeof(work), "%s", line);
		count = tokenize(work, tok);
		for (int i = 2; i < count; i++)
			if (strncmp(tok[i], "A=", 2) == 0) {
				aat = i;
				break;
			}
		if (aat > 0) {
			struct row *r = &rows[n];

			memset(r, 0, sizeof(*r));
			copy_field(r->user, NAME_LEN, tok[0]);
			copy_field(r->session, sizeof(r->session), "TSO/E");
			r->status[0] = '\0';
			for (int i = 1; i < aat; i++) {
				size_t used = strlen(r->status);

				snprintf(r->status + used, STATUS_LEN - used,
					 "%s%s", used > 0 ? " " : "", tok[i]);
			}
			copy_field(r->asid, ASID_LEN, tok[aat] + 2);
			copy_field(r->cpu, CPU_LEN, "NA");
			copy_field(r->elapsed, TIME_LEN, "NA");
			copy_field(r->scl, SCL_LEN, "NA");
			copy_field(r->prio, PRIO_LEN, "NA");
			copy_field(r->rgp, RGP_LEN, "NA");
			n++;
		}
		if (n == 0) {
			line = next;
			continue;
		}
		{
			struct row *r = &rows[n - 1];

			for (int i = 0; i < count; i++) {
				if (strncmp(tok[i], "CT=", 3) == 0) {
					copy_field(r->cpu, CPU_LEN, tok[i] + 3);
					{
						size_t len = strlen(r->cpu);

						if (len > 1 &&
						    r->cpu[len - 1] == 'S')
							r->cpu[len - 1] = '\0';
					}
				} else if (strncmp(tok[i], "ET=", 3) == 0) {
					fmt_clock(tok[i] + 3, r->elapsed,
						  TIME_LEN);
				} else if (strncmp(tok[i], "WUID=", 5) == 0) {
					copy_field(r->wuid, WUID_LEN, tok[i] + 5);
				} else if (strncmp(tok[i], "SCL=", 4) == 0) {
					copy_field(r->scl, SCL_LEN, tok[i] + 4);
				} else if (strncmp(tok[i], "P=", 2) == 0) {
					copy_field(r->prio, PRIO_LEN, tok[i] + 2);
				} else if (strncmp(tok[i], "RGP=", 4) == 0) {
					copy_field(r->rgp, RGP_LEN, tok[i] + 4);
				}
			}
		}
		line = next;
	}
	return n;
}

/* PID of the OMVS process in this address space, or "NA" for a TSO/E
 * session that has no OMVS process at all. */
static void set_pid(const struct proc *procs, int nprocs, struct row *r)
{
	for (int i = 0; i < nprocs; i++) {
		if (strcmp(procs[i].asid, r->asid) == 0) {
			snprintf(r->pid, PID_LEN, "%ld", procs[i].pid);
			return;
		}
	}
	copy_field(r->pid, PID_LEN, "NA");
}

static int add_ssh_rows(const struct proc *procs, int nprocs, struct row *rows,
			int nrows, int max)
{
	for (int i = 0; i < nprocs && nrows < max; i++) {
		if (strstr(procs[i].cmd, "sshd") == NULL)
			continue;
		if (is_daemon(procs, nprocs, procs[i].user))
			continue;
		copy_field(rows[nrows].user, NAME_LEN, procs[i].user);
		copy_field(rows[nrows].session, sizeof(rows[nrows].session), "SSH");
		copy_field(rows[nrows].status, sizeof(rows[nrows].status), "-");
		copy_field(rows[nrows].asid, ASID_LEN, procs[i].asid);
		copy_field(rows[nrows].wuid, WUID_LEN, "NA");
		copy_field(rows[nrows].cpu, CPU_LEN, "NA");
		copy_field(rows[nrows].scl, SCL_LEN, "NA");
		copy_field(rows[nrows].prio, PRIO_LEN, "NA");
		copy_field(rows[nrows].rgp, RGP_LEN, "NA");
		snprintf(rows[nrows].pid, PID_LEN, "%ld", procs[i].pid);
		format_elapsed(procs[i].start, rows[nrows].elapsed, TIME_LEN);
		nrows++;
	}
	return nrows;
}

static int compare_rows(const void *a, const void *b)
{
	const struct row *x = a;
	const struct row *y = b;
	int cmp = strcmp(x->user, y->user);

	if (cmp != 0)
		return cmp;
	cmp = strcmp(x->session, y->session);
	if (cmp != 0)
		return cmp;
	return strcmp(x->asid, y->asid);
}

/* Run one display; returns 0 and a NUL-terminated copy of stdout, or -1.
 * EBCDIC, and quoted so that the whole command is one argv element. */
static int run_display(zoautil_vector4_t *vector, const char *args,
		       zoautil_options_t options, char **out)
{
	zoau_response *response = NULL;
	char parms[MAX_ARGS];
	size_t len;
	int rc;

	*out = NULL;
	snprintf(parms, sizeof(parms), "\"%s\"", args);
	len = strlen(parms);
	rc = vector->opercmd(parms, len, &response, options);
	if (rc != 0 || response == NULL) {
		fprintf(stderr, "whoson: %s failed, rc = %d\n", args, rc);
		if (response != NULL)
			fprintf(stderr,
				"whoson: cmd_rc = %d, stdout_len = %d, "
				"stderr_len = %d\n", response->cmd_rc,
				response->stdout_len, response->stderr_len);
		if (response != NULL && response->stderr_response != NULL &&
		    response->stderr_len > 0)
			fprintf(stderr, "%.*s\n", response->stderr_len,
				response->stderr_response);
		if (response != NULL)
			free_response(response);
		return -1;
	}
	if (response->stdout_response == NULL || response->stdout_len <= 0) {
		fprintf(stderr, "whoson: %s returned no output\n", args);
		free_response(response);
		return -1;
	}
	*out = malloc((size_t)response->stdout_len + 1);
	if (*out == NULL) {
		fprintf(stderr, "whoson: out of memory\n");
		free_response(response);
		return -1;
	}
	memcpy(*out, response->stdout_response, (size_t)response->stdout_len);
	(*out)[response->stdout_len] = '\0';
	free_response(response);
	return 0;
}

int main(int argc, char *argv[])
{
	static struct proc procs[MAX_PROCS];
	static struct row rows[MAX_ROWS];
	zoautil_options_t options = {
		ZOAUTIL_EYECATCHER,
		VERSION_CURRENT,
		FORMAT_TEXT
	};
	zoautil_vector4_t *vector;
	char *ts_text = NULL;
	char *omvs_text = NULL;
	unsigned int vrm;
	int nprocs, nrows = 0;
	int rc = 1;

	if (argc > 1) {
		if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
			printf("usage: whoson\n\n"
			       "List logged-on users with session type "
			       "(TSO/E, SSH).\n");
			return 0;
		}
		return usage_error("unknown argument");
	}

	vrm = version();
	if (vrm < VERSION_CURRENT) {
		fprintf(stderr,
			"whoson: ZOAU shared library is too old: 0x%x < 0x%x\n",
			vrm, VERSION_CURRENT);
		return 1;
	}
	vector = init(vrm);
	if (vector == NULL) {
		fprintf(stderr, "whoson: cannot get the ZOAU vector table\n");
		return 1;
	}
	if (run_display(vector, "D OMVS,A=ALL", options, &omvs_text) != 0)
		goto out;
	if (run_display(vector, "D TS,A", options, &ts_text) != 0)
		goto out;

	nprocs = parse_omvs(omvs_text, procs, MAX_PROCS);
	nrows = parse_ts(ts_text, rows, MAX_ROWS);
	for (int i = 0; i < nrows; i++)
		set_pid(procs, nprocs, &rows[i]);
	nrows = add_ssh_rows(procs, nprocs, rows, nrows, MAX_ROWS);
	qsort(rows, (size_t)nrows, sizeof(rows[0]), compare_rows);

	printf("%-8s %-6s %-6s %-4s %-11s %-10s %-8s %-10s %-4s %-4s %-6s\n",
	       "USER", "TYPE", "STATUS", "ASID", "PID", "WUID", "CPU",
	       "ELAPSED", "SCL", "PRIO", "RGP");
	for (int i = 0; i < nrows; i++)
		printf("%-8s %-6s %-6s %-4s %-11s %-10s %-8s %-10s %-4s %-4s %-6s\n",
		       rows[i].user, rows[i].session, rows[i].status,
		       rows[i].asid, rows[i].pid, rows[i].wuid, rows[i].cpu,
		       rows[i].elapsed, rows[i].scl, rows[i].prio,
		       rows[i].rgp);
	rc = 0;
out:
	free(ts_text);
	free(omvs_text);
	term(vector);
	return rc;
}
