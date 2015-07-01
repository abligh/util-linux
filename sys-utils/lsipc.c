/*
 * lsipc - List information about IPC instances employed in the system
 *
 * Copyright (C) 2015 Ondrej Oprala <ooprala@redhat.com>
 * Copyright (C) 2015 Karel Zak <ooprala@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 *
 * lsipc is inspired by the ipcs utility. The aim is to create
 * a utility unencumbered by a standard to provide more flexible
 * means of controlling the output.
 */

#include <errno.h>
#include <features.h>
#include <getopt.h>
#include <sys/time.h>
#include <unistd.h>

#include <libsmartcols.h>

#include "c.h"
#include "nls.h"
#include "closestream.h"
#include "strutils.h"
#include "optutils.h"
#include "xalloc.h"
#include "procutils.h"
#include "ipcutils.h"

/*
 * time modes
 * */
enum {
	TIME_INVALID = 0,
	TIME_SHORT,
	TIME_FULL,
	TIME_ISO,
};

/*
 * IDs
 */
enum {
	/* generic */
	COLDESC_IDX_GEN_FIRST = 0,
		COL_KEY = COLDESC_IDX_GEN_FIRST,
		COL_ID,
		COL_OWNER,
		COL_PERMS,
		COL_CUID,
		COL_CGID,
		COL_UID,
		COL_GID,
		COL_CTIME,
	COLDESC_IDX_GEN_LAST = COL_CTIME,

	/* msgq-specific */
	COLDESC_IDX_MSG_FIRST,
		COL_USEDBYTES = COLDESC_IDX_MSG_FIRST,
		COL_MSGS,
		COL_SEND,
		COL_RECV,
		COL_LSPID,
		COL_LRPID,
	COLDESC_IDX_MSG_LAST = COL_LRPID,

	/* shm-specific */
	COLDESC_IDX_SHM_FIRST,
		COL_SIZE = COLDESC_IDX_SHM_FIRST,
		COL_NATTCH,
		COL_STATUS,
		COL_ATTACH,
		COL_DETACH,
		COL_COMMAND,
		COL_CPID,
		COL_LPID,
	COLDESC_IDX_SHM_LAST = COL_LPID,

	/* sem-specific */
	COLDESC_IDX_SEM_FIRST,
		COL_NSEMS = COLDESC_IDX_SEM_FIRST,
		COL_OTIME,
	COLDESC_IDX_SEM_LAST = COL_OTIME,

	/* summary (--global) */
	COLDESC_IDX_SUM_FIRST,
		COL_RESOURCE = COLDESC_IDX_SUM_FIRST,
		COL_DESC,
		COL_USED,
		COL_LIMIT,
	COLDESC_IDX_SUM_LAST = COL_LIMIT
};

/* we use the value of outmode to determine
 * appropriate flags for the libsmartcols table
 * (e.g., a value of out_newline would imply a raw
 * table with the column separator set to '\n').
 */
static int outmode;

/* not all columns apply to all options, so we specify a legal range for each */
static size_t LOWER, UPPER;

/*
 * output modes
 */
enum {
	OUT_COLON = 1,
	OUT_EXPORT,
	OUT_NEWLINE,
	OUT_RAW,
	OUT_NUL,
	OUT_PRETTY
};

struct lsipc_control {
	unsigned int noheadings : 1,		/* don't print header line */
		     notrunc : 1,		/* don't truncate columns */
		     json : 1,			/* JSON output */
		     bytes : 1,			/* SIZE in bytes */
		     numperms : 1,		/* numeric permissions */
		     time_mode : 2;
};

struct lsipc_coldesc {
	const char *name;
	const char *help;
	const char *pretty_name;

	double whint;	/* width hint */
	long flag;
};

static const struct lsipc_coldesc coldescs[] =
{
	/* common */
	[COL_KEY]	= { "KEY",	N_("Resource key"), N_("Key"), 1},
	[COL_ID]	= { "ID",	N_("Resource ID"), N_("ID"), 1},
	[COL_OWNER]	= { "OWNER",	N_("Owner"), N_("Owner"), 1, SCOLS_FL_RIGHT},
	[COL_PERMS]	= { "PERMS",	N_("Permissions"), N_("Permissions"), 1, SCOLS_FL_RIGHT},
	[COL_CUID]	= { "CUID",	N_("Creator UID"), N_("CUID"), 1, SCOLS_FL_RIGHT},
	[COL_CGID]	= { "CGID",	N_("Creator GID"), N_("CGID"), 1, SCOLS_FL_RIGHT},
	[COL_UID]	= { "UID",	N_("User ID"), N_("UID"), 1, SCOLS_FL_RIGHT},
	[COL_GID]	= { "GID",	N_("Group ID"), N_("GID"), 1, SCOLS_FL_RIGHT},
	[COL_CTIME]	= { "CTIME",	N_("Time of the last change"), N_("Last change"), 1, SCOLS_FL_RIGHT},

	/* msgq-specific */
	[COL_USEDBYTES]	= { "USEDBYTES",N_("Bytes used"), N_("Bytes used"), 1, SCOLS_FL_RIGHT},
	[COL_MSGS]	= { "MSGS",	N_("Number of messages"), N_("Messages"), 1},
	[COL_SEND]	= { "SEND",	N_("Time of last msg sent"), N_("Msg sent"), 1, SCOLS_FL_RIGHT},
	[COL_RECV]	= { "RECV",	N_("Time of last msg received"), N_("Msg received"), 1, SCOLS_FL_RIGHT},
	[COL_LSPID]	= { "LSPID",	N_("PID of the last msg sender"), N_("Msg sender"), 1, SCOLS_FL_RIGHT},
	[COL_LRPID]	= { "LRPID",	N_("PID of the last msg receiver"), N_("Msg receiver"), 1, SCOLS_FL_RIGHT},

	/* shm-specific */
	[COL_SIZE]	= { "SIZE",	N_("Segment size"), N_("Segment size"), 1, SCOLS_FL_RIGHT},
	[COL_NATTCH]	= { "NATTCH",	N_("Number of attached processes"), N_("Attached processes"), 1, SCOLS_FL_RIGHT},
	[COL_STATUS]	= { "STATUS",	N_("Status"), N_("Status"), 1, SCOLS_FL_NOEXTREMES},
	[COL_ATTACH]	= { "ATTACH",	N_("Attach time"), N_("Attach time"), 1, SCOLS_FL_RIGHT},
	[COL_DETACH]	= { "DETACH",	N_("Detach time"), N_("Detach time"), 1, SCOLS_FL_RIGHT},
	[COL_COMMAND]	= { "COMMAND",  N_("Creator command line"), N_("Creator command"), 0, SCOLS_FL_TRUNC},
	[COL_CPID]	= { "CPID",	N_("PID of the creator"), N_("Creator PID"), 1, SCOLS_FL_RIGHT},
	[COL_LPID]	= { "LPID",	N_("PID of last user"), N_("Last user PID"), 1, SCOLS_FL_RIGHT},

	/* sem-specific */
	[COL_NSEMS]	= { "NSEMS",	N_("Number of semaphores"), N_("Semaphores"), 1, SCOLS_FL_RIGHT},
	[COL_OTIME]	= { "OTIME",	N_("Time of the last operation"), N_("Last operation"), 1, SCOLS_FL_RIGHT},

	/* cols for summarized information */
	[COL_RESOURCE]  = { "RESOURCE", N_("Resource name"), N_("Resource"), 1 },
	[COL_DESC]      = { "DESCRIPTION",N_("Resource description"), N_("Description"), 1 },
	[COL_USED]      = { "USED",     N_("Currently used"), N_("Used"), 1, SCOLS_FL_RIGHT },
	[COL_LIMIT]     = { "LIMIT",    N_("System-wide limit"), N_("Limit"), 1, SCOLS_FL_RIGHT },
};


/* columns[] array specifies all currently wanted output column. The columns
 * are defined by coldescs[] array and you can specify (on command line) each
 * column twice. That's enough, dynamically allocated array of the columns is
 * unnecessary overkill and over-engineering in this case */
static int columns[ARRAY_SIZE(coldescs) * 2];
static int ncolumns;

static inline size_t err_columns_index(size_t arysz, size_t idx)
{
	if (idx >= arysz)
		errx(EXIT_FAILURE, _("too many columns specified, "
				     "the limit is %zu columns"),
				arysz - 1);
	return idx;
}

#define add_column(ary, n, id)	\
		((ary)[ err_columns_index(ARRAY_SIZE(ary), (n)) ] = (id))

static int column_name_to_id(const char *name, size_t namesz)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(coldescs); i++) {
		const char *cn = coldescs[i].name;

		if (!strncasecmp(name, cn, namesz) && !*(cn + namesz)) {
			if (i > COL_CTIME) {
				if (i >= LOWER && i <= UPPER)
					return i;
				else {
					warnx(_("column %s does not apply to the specified IPC"), name);
					return -1;
				}
			} else
				return i;
		}
	}
	warnx(_("unknown column: %s"), name);
	return -1;
}

static int parse_time_mode(const char *optarg)
{
	struct lsipc_timefmt {
		const char *name;
		const int val;
	};
	static const struct lsipc_timefmt timefmts[] = {
		{"iso", TIME_ISO},
		{"full", TIME_FULL},
		{"short", TIME_SHORT},
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(timefmts); i++) {
		if (strcmp(timefmts[i].name, optarg) == 0)
			return timefmts[i].val;
	}
	errx(EXIT_FAILURE, _("unknown time format: %s"), optarg);
}

static void __attribute__ ((__noreturn__)) usage(FILE * out)
{
	size_t i;

	fputs(USAGE_HEADER, out);
	fputs(USAGE_HEADER, out);
	fprintf(out, _(" %s [options]\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Show information on IPC facilities.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(USAGE_HELP, out);
	fputs(USAGE_VERSION, out);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Resource options:\n"), out);
	fputs(_(" -m, --shmems      shared memory segments\n"), out);
	fputs(_(" -q, --queues      message queues\n"), out);
	fputs(_(" -s, --semaphores  semaphores\n"), out);
	fputs(_(" -g, --global      info about system-wide usage (may be used with -m, -q and -s)\n"), out);
	fputs(_(" -i, --id <id>     print details on resource identified by <id>\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(_("     --colon-separate     display data in a format similar to /etc/passwd\n"), out);
	fputs(_("     --noheadings         don't print headings\n"), out);
	fputs(_("     --notruncate         don't truncate output\n"), out);
	fputs(_("     --time-format=<type> display dates in short, full or iso format\n"), out);
	fputs(_(" -b, --bytes              print SIZE in bytes rather than in human readable format\n"), out);
	fputs(_(" -c, --creator            show creator and owner\n"), out);
	fputs(_(" -e, --export             display in an export-able output format\n"), out);
	fputs(_(" -J, --json               use the JSON output format\n"), out);
	fputs(_(" -n, --newline            display each piece of information on a new line\n"), out);
	fputs(_(" -o, --output[=<list>]    define the columns to output\n"), out);
	fputs(_(" -P, --numeric-perms      print numeric permissions (PERMS column)\n"), out);
	fputs(_(" -r, --raw                display in raw mode\n"), out);
	fputs(_(" -t, --time               show attach, detach and change times\n"), out);
	fputs(_(" -z, --print0             delimit user entries with a nul character\n"), out);

	fprintf(out, _("\nGeneric columns:\n"));
	for (i = COLDESC_IDX_GEN_FIRST; i <= COLDESC_IDX_GEN_LAST; i++)
		fprintf(out, " %14s  %s\n", coldescs[i].name, _(coldescs[i].help));

	fprintf(out, _("\nShared memory columns (--shmems):\n"));
	for (i = COLDESC_IDX_SHM_FIRST; i <= COLDESC_IDX_SHM_LAST; i++)
		fprintf(out, " %14s  %s\n", coldescs[i].name, _(coldescs[i].help));

	fprintf(out, _("\nMessages queues columns (--queues):\n"));
	for (i = COLDESC_IDX_MSG_FIRST; i <= COLDESC_IDX_MSG_LAST; i++)
		fprintf(out, " %14s  %s\n", coldescs[i].name, _(coldescs[i].help));

	fprintf(out, _("\nSemaphores columns (--semaphores):\n"));
	for (i = COLDESC_IDX_SEM_FIRST; i <= COLDESC_IDX_SEM_LAST; i++)
		fprintf(out, " %14s  %s\n", coldescs[i].name, _(coldescs[i].help));

	fprintf(out, _("\nSummary columns (--global):\n"));
	for (i = COLDESC_IDX_SUM_FIRST; i <= COLDESC_IDX_SUM_LAST; i++)
		fprintf(out, " %14s  %s\n", coldescs[i].name, _(coldescs[i].help));

	fprintf(out, USAGE_MAN_TAIL("lsipc(1)"));
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

static struct libscols_table *setup_table(struct lsipc_control *ctl)
{
	struct libscols_table *table = scols_new_table();
	int n = 0;

	if (!table)
		errx(EXIT_FAILURE, _("failed to initialize output table"));
	if (ctl->noheadings)
		scols_table_enable_noheadings(table, 1);
	if (ctl->json)
		scols_table_enable_json(table, 1);

	switch(outmode) {
	case OUT_COLON:
		scols_table_enable_raw(table, 1);
		scols_table_set_column_separator(table, ":");
		break;
	case OUT_NEWLINE:
		scols_table_set_column_separator(table, "\n");
		/* fallthrough */
	case OUT_EXPORT:
		scols_table_enable_export(table, 1);
		break;
	case OUT_NUL:
		scols_table_set_line_separator(table, "\0");
		/* fallthrough */
	case OUT_RAW:
		scols_table_enable_raw(table, 1);
		break;
	case OUT_PRETTY:
		scols_table_enable_noheadings(table, 1);
	default:
		break;
	}

	while (n < ncolumns) {
		int flags = coldescs[columns[n]].flag;

		if (ctl->notrunc)
			flags &= ~SCOLS_FL_TRUNC;

		if (!scols_table_new_column(table,
				coldescs[columns[n]].name,
				coldescs[columns[n]].whint,
				flags))
			goto fail;
		++n;
	}
	return table;
fail:
	scols_unref_table(table);
	return NULL;
}

static int print_pretty(struct libscols_table *table)
{
	struct libscols_iter *itr = scols_new_iter(SCOLS_ITER_FORWARD);
	struct libscols_column *col;
	struct libscols_cell *data;
	struct libscols_line *ln;
	const char *hstr, *dstr, *estr;
	int n = 0;

	ln = scols_table_get_line(table, 0);
	while (!scols_table_next_column(table, itr, &col)) {

		data = scols_line_get_cell(ln, n);

		hstr = N_(coldescs[columns[n]].pretty_name);
		dstr = scols_cell_get_data(data);

		if (dstr)
			printf("%s:%*c%-36s\n", hstr, 35 - (int)strlen(hstr), ' ', dstr);
		++n;
	}

	/* this is used to pretty-print detailed info about a semaphore array */
	if (ln) {
		estr = scols_line_get_userdata(ln);
		if (estr) {
			printf("%s", (const char *)estr);
			free((void *)estr);
		}
	}

	scols_free_iter(itr);
	return 0;

}

static int print_table(struct libscols_table *tb)
{
	if (outmode == OUT_PRETTY)
		print_pretty(tb);
	else
		scols_print_table(tb);
	return 0;
}
static struct timeval now;

static int date_is_today(time_t t)
{
	if (now.tv_sec == 0)
		gettimeofday(&now, NULL);
	return t / (3600 * 24) == now.tv_sec / (3600 * 24);
}

static int date_is_thisyear(time_t t)
{
	if (now.tv_sec == 0)
		gettimeofday(&now, NULL);
	return t / (3600 * 24 * 365) == now.tv_sec / (3600 * 24 * 365);
}

static char *make_time(int mode, time_t time)
{
	char *s;
	struct tm tm;
	char buf[64] = {0};

	localtime_r(&time, &tm);

	switch(mode) {
	case TIME_FULL:
		asctime_r(&tm, buf);
		if (*(s = buf + strlen(buf) - 1) == '\n')
			*s = '\0';
		break;
	case TIME_SHORT:
		if (date_is_today(time))
			strftime(buf, sizeof(buf), "%H:%M", &tm);
		else if (date_is_thisyear(time))
			strftime(buf, sizeof(buf), "%b%d", &tm);
		else
			strftime(buf, sizeof(buf), "%Y-%b%d", &tm);
		break;
	case TIME_ISO:
		strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
		break;
	default:
		errx(EXIT_FAILURE, _("unsupported time type"));
	}
	return xstrdup(buf);
}

static void global_set_data(struct libscols_table *tb, const char *resource,
			    const char *desc, uintmax_t used, uintmax_t limit)
{
	struct libscols_line *ln;
	int n;

	ln = scols_table_new_line(tb, NULL);
	if (!ln)
		err_oom();

	for (n = 0; n < ncolumns; n++) {
		int rc = 0;
		char *arg = NULL;

		switch (columns[n]) {
		case COL_RESOURCE:
			rc = scols_line_set_data(ln, n, resource);
			break;
		case COL_DESC:
			rc = scols_line_set_data(ln, n, desc);
			break;
		case COL_USED:
			xasprintf(&arg, "%ju", used);
			rc = scols_line_set_data(ln, n, arg);
			break;
		case COL_LIMIT:
			xasprintf(&arg, "%ju", limit);
			rc = scols_line_set_data(ln, n, arg);
			break;
		}

		if (rc != 0)
			err(EXIT_FAILURE, _("failed to set data"));
		free(arg);
	}
}

static void do_sem(int id, struct lsipc_control *ctl, struct libscols_table *tb)
{
	int n = 0;
	struct libscols_line *ln;
	struct passwd *pw = NULL, *cpw = NULL;
	struct group *gr = NULL, *cgr = NULL;
	struct sem_data *semds, *semdsp;
	char *arg = NULL, *time;

	if (ipc_sem_get_info(id, &semds) < 1) {
		if (id > -1)
			warnx(_("id %d not found"), id);
		return;
	}
	for (semdsp = semds;  semdsp->next != NULL || id > -1; semdsp = semdsp->next) {
		ln = scols_table_new_line(tb, NULL);

		/* no need to call getpwuid() for the same user */
		if (!(pw && pw->pw_uid == semdsp->sem_perm.uid))
			pw = getpwuid(semdsp->sem_perm.uid);

		/* no need to call getgrgid() for the same group */
		if (!(gr && gr->gr_gid == semdsp->sem_perm.gid))
			gr = getgrgid(semdsp->sem_perm.gid);

		/* no need to call getpwuid() for the same user */
		if (!(cpw && cpw->pw_uid == semdsp->sem_perm.cuid))
			cpw = getpwuid(semdsp->sem_perm.cuid);

		/* no need to call getcgrgid() for the same cgroup */
		if (!(cgr && cgr->gr_gid == semdsp->sem_perm.cgid))
			cgr = getgrgid(semdsp->sem_perm.cgid);
		n = 0;
		while (n < ncolumns) {
			int rc = 0;

			switch (columns[n]) {
				case COL_KEY:
					xasprintf(&arg, "0x%08x",semdsp->sem_perm.key);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_ID:
					xasprintf(&arg, "%d",semdsp->sem_perm.id);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_OWNER:
					if (pw)
						xasprintf(&arg, "%s", pw->pw_name);
					else
						xasprintf(&arg, "%u", semdsp->sem_perm.uid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_PERMS:
					if (ctl->numperms)
						xasprintf(&arg, "%#o", semdsp->sem_perm.mode & 0777);
					else {
						arg = xmalloc(11);
						strmode(semdsp->sem_perm.mode & 0777, arg);
					}
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_CUID:
					if (cpw)
						xasprintf(&arg, "%s", cpw->pw_name);
					else
						xasprintf(&arg, "%u", semdsp->sem_perm.cuid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_CGID:
					if (cgr)
						xasprintf(&arg, "%s", cgr->gr_name);
					else
						xasprintf(&arg, "%u", semdsp->sem_perm.cuid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_UID:
					if (pw)
						xasprintf(&arg, "%s", pw->pw_name);
					else
						xasprintf(&arg, "%u", semdsp->sem_perm.uid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_GID:
					if (gr)
						xasprintf(&arg, "%s", gr->gr_name);
					else
						xasprintf(&arg, "%u", semdsp->sem_perm.gid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_CTIME:
					if (semdsp->sem_ctime != 0) {
						rc = scols_line_set_data(ln, n,
							time = make_time(ctl->time_mode,
								  (time_t)semdsp->sem_ctime));
						free(time);
					}
					break;
				case COL_NSEMS:
					xasprintf(&arg, "%ju", semdsp->sem_nsems);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_OTIME:
					if (semdsp->sem_otime != 0) {
						rc = scols_line_set_data(ln, n,
							time = make_time(ctl->time_mode,
								  (time_t)semdsp->sem_otime));
						free(time);
					}
					break;
			}
			if (rc != 0)
				err(EXIT_FAILURE, _("failed to set data"));
			++n;
			free(arg);
			arg = NULL;
		}
		/* no empty element when ID was specified */
		if (id > -1) {
			size_t i = 0, offt;
			/* line length * (header + nsems) + '\0' */
			char *data = xcalloc(1, sizeof(char) * 55 * (semds->sem_nsems + 1) + 1);

			/* same as in ipcs.c */
			offt = sprintf(data, "%-10s %-10s %-10s %-10s %-10s\n",
			       _("semnum"), _("value"), _("ncount"), _("zcount"), _("pid"));

			for (i = 0; i < semds->sem_nsems; i++) {
				struct sem_elem *e = &semds->elements[i];
				offt += sprintf(data + offt, "%-10zd %-10d %-10d %-10d %-10d\n",
				       i, e->semval, e->ncount, e->zcount, e->pid);
			}
			scols_line_set_userdata(ln, (void *)data);
			break;
		}
	}
	ipc_sem_free_info(semds);
}

static void do_sem_global(struct libscols_table *tb)
{
	struct sem_data *semds, *semdsp;
	struct ipc_limits lim;
	int nsems = 0, nsets = 0;

	ipc_sem_get_limits(&lim);

	if (ipc_sem_get_info(-1, &semds) > 0) {
		for (semdsp = semds; semdsp->next != NULL; semdsp = semdsp->next) {
			++nsets;
			nsems += semds->sem_nsems;
		}
		ipc_sem_free_info(semds);
	}

	global_set_data(tb, "SEMMNS", _("Total number of semaphores"), nsems, lim.semmns);
	global_set_data(tb, "SEMMNI", _("Number of Semaphore IDs"), nsets, lim.semmni);
}

static void do_msg(int id, struct lsipc_control *ctl, struct libscols_table *tb)
{
	int n = 0;
	struct libscols_line *ln;
	struct passwd *pw = NULL;
	struct group *gr = NULL;
	struct msg_data *msgds, *msgdsp;
	char *arg = NULL, *time;

	if (ipc_msg_get_info(id, &msgds) < 1) {
		if (id > -1)
			warnx(_("id %d not found"), id);
		return;
	}

	for (msgdsp = msgds; msgdsp->next != NULL || id > -1 ; msgdsp = msgdsp->next) {

		ln = scols_table_new_line(tb, NULL);

		/* no need to call getpwuid() for the same user */
		if (!(pw && pw->pw_uid == msgdsp->msg_perm.uid))
			pw = getpwuid(msgdsp->msg_perm.uid);

		/* no need to call getgrgid() for the same user */
		if (!(gr && gr->gr_gid == msgdsp->msg_perm.gid))
			gr = getgrgid(msgdsp->msg_perm.gid);

		n = 0;
		while (n < ncolumns) {
			int rc = 0;

			switch (columns[n]) {
				case COL_KEY:
					xasprintf(&arg, "0x%08x",msgdsp->msg_perm.key);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_ID:
					xasprintf(&arg, "%d",msgdsp->msg_perm.id);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_OWNER:
					if (pw)
						xasprintf(&arg, "%s", pw->pw_name);
					else
						xasprintf(&arg, "%u", msgdsp->msg_perm.uid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_PERMS:
					if (ctl->numperms)
						xasprintf(&arg, "%#o", msgdsp->msg_perm.mode & 0777);
					else {
						arg = xmalloc(11);
						strmode(msgdsp->msg_perm.mode & 0777, arg);
						rc = scols_line_set_data(ln, n, arg);
					}
					break;
				case COL_CUID:
					if (msgdsp->msg_perm.cuid == msgdsp->msg_perm.uid
					    || (pw = getpwuid(msgdsp->msg_perm.cuid)))
						xasprintf(&arg, "%s", pw->pw_name);
					else
						xasprintf(&arg, "%u", msgdsp->msg_perm.cuid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_CGID:
					if (msgdsp->msg_perm.cgid == msgdsp->msg_perm.gid
					    || (gr = getgrgid(msgdsp->msg_perm.cgid)))
						xasprintf(&arg, "%s", gr->gr_name);
					else
						xasprintf(&arg, "%u", msgdsp->msg_perm.cuid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_UID:
					if (pw)
						xasprintf(&arg, "%s", pw->pw_name);
					else
						xasprintf(&arg, "%u", msgdsp->msg_perm.uid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_GID:
					if (gr)
						xasprintf(&arg, "%s", gr->gr_name);
					else
						xasprintf(&arg, "%u", msgdsp->msg_perm.gid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_CTIME:
					if (msgdsp->q_ctime != 0) {
						rc = scols_line_set_data(ln, n,
							time = make_time(ctl->time_mode,
								  (time_t)msgdsp->q_ctime));
						free(time);
					}
					break;
				case COL_USEDBYTES:
					xasprintf(&arg, "%ju", msgdsp->q_cbytes);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_MSGS:
					xasprintf(&arg, "%ju", msgdsp->q_qnum);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_SEND:
					if (msgdsp->q_stime != 0) {
						rc = scols_line_set_data(ln, n,
							time = make_time(ctl->time_mode,
								  (time_t)msgdsp->q_stime));
						free(time);
					}
					break;
				case COL_RECV:
					if (msgdsp->q_rtime != 0) {
						rc = scols_line_set_data(ln, n,
							time = make_time(ctl->time_mode,
								  (time_t)msgdsp->q_rtime));
						free(time);
					}
					break;
				case COL_LSPID:
					xasprintf(&arg, "%u", msgdsp->q_lspid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_LRPID:
					xasprintf(&arg, "%u", msgdsp->q_lrpid);
					rc = scols_line_set_data(ln, n, arg);
					break;
			}
			if (rc != 0)
				err(EXIT_FAILURE, _("failed to set data"));
			++n;
			free(arg);
			arg = NULL;
		}
		if (id > -1)
			break;
	}
	ipc_msg_free_info(msgds);
}


static void do_msg_global(struct libscols_table *tb)
{
	struct msg_data *msgds, *msgdsp;
	struct ipc_limits lim;
	int msgqs = 0;

	ipc_msg_get_limits(&lim);

	/* count number of used queues */
	if (ipc_msg_get_info(-1, &msgds) > 0) {
		for (msgdsp = msgds; msgdsp->next != NULL; msgdsp = msgdsp->next)
			++msgqs;
		ipc_msg_free_info(msgds);
	}

	global_set_data(tb, "MSGMNI", _("Number of message queues"), msgqs, lim.msgmni);
	global_set_data(tb, "MSGMAX", _("Max size of message (bytes)"),	0, lim.msgmax);
	global_set_data(tb, "MSGMNB", _("Default max size of queue (bytes)"), 0, lim.msgmnb);
}

static void do_shm(int id, struct lsipc_control *ctl, struct libscols_table *tb)
{
	int n = 0;
	struct libscols_line *ln;
	struct passwd *pw = NULL;
	struct group *gr = NULL;
	struct shm_data *shmds, *shmdsp;
	char *arg = NULL, *time;

	if (ipc_shm_get_info(id, &shmds) < 1) {
		if (id > -1)
			warnx(_("id %d not found"), id);
		return;
	}

	for (shmdsp = shmds; shmdsp->next != NULL || id > -1 ; shmdsp = shmdsp->next) {

		ln = scols_table_new_line(tb, NULL);
		if (!ln)
			err_oom();

		/* no need to call getpwuid() for the same user */
		if (!(pw && pw->pw_uid == shmdsp->shm_perm.uid))
			pw = getpwuid(shmdsp->shm_perm.uid);

		/* no need to call getgrgid() for the same user */
		if (!(gr && gr->gr_gid == shmdsp->shm_perm.gid))
			gr = getgrgid(shmdsp->shm_perm.gid);

		n = 0;
		while (n < ncolumns) {
			int rc = 0;

			switch (columns[n]) {
				case COL_KEY:
					xasprintf(&arg, "0x%08x",shmdsp->shm_perm.key);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_ID:
					xasprintf(&arg, "%d",shmdsp->shm_perm.id);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_OWNER:
					if (pw)
						xasprintf(&arg, "%s", pw->pw_name);
					else
						xasprintf(&arg, "%u", shmdsp->shm_perm.uid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_PERMS:
					if (ctl->numperms)
						xasprintf(&arg, "%#o", shmdsp->shm_perm.mode & 0777);
					else {
						arg = xmalloc(11);
						strmode(shmdsp->shm_perm.mode & 0777, arg);
					}
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_CUID:
					if (shmdsp->shm_perm.cuid == shmdsp->shm_perm.uid
							|| (pw = getpwuid(shmdsp->shm_perm.cuid)))
						xasprintf(&arg, "%s", pw->pw_name);
					else
						xasprintf(&arg, "%u", shmdsp->shm_perm.cuid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_CGID:
					if (shmdsp->shm_perm.cgid == shmdsp->shm_perm.gid
							|| (gr = getgrgid(shmdsp->shm_perm.cgid)))
						xasprintf(&arg, "%s", gr->gr_name);
					else
						xasprintf(&arg, "%u", shmdsp->shm_perm.cuid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_UID:
					if (pw)
						xasprintf(&arg, "%s", pw->pw_name);
					else
						xasprintf(&arg, "%u", shmdsp->shm_perm.uid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_GID:
					if (gr)
						xasprintf(&arg, "%s", gr->gr_name);
					else
						xasprintf(&arg, "%u", shmdsp->shm_perm.gid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_CTIME:
					if (shmdsp->shm_ctim != 0) {
						rc = scols_line_set_data(ln, n,
							time = make_time(ctl->time_mode,
								  (time_t)shmdsp->shm_ctim));
						free(time);
					}
					break;
				case COL_SIZE:
					if (ctl->bytes)
						xasprintf(&arg, "%ju", shmdsp->shm_segsz);
					else
						arg = size_to_human_string(SIZE_SUFFIX_1LETTER, shmdsp->shm_segsz);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_NATTCH:
					xasprintf(&arg, "%ju", shmdsp->shm_nattch);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_STATUS: {
						int comma = 0;
						size_t offt = 0;
						free (arg);
						arg = xcalloc(1, sizeof(char) * strlen(_("dest"))
								+ strlen(_("locked"))
								+ strlen(_("hugetlb"))
								+ strlen(_("noreserve")) + 4);

						if (shmdsp->shm_perm.mode & SHM_DEST) {
							offt += sprintf(arg, "%s", _("dest"));
							comma++;
						}
						if (shmdsp->shm_perm.mode & SHM_LOCKED) {
							if (comma)
								arg[offt++] = ',';
							offt += sprintf(arg + offt, "%s", _("locked"));
						}
						if (shmdsp->shm_perm.mode & SHM_HUGETLB) {
							if (comma)
								arg[offt++] = ',';
							offt += sprintf(arg + offt, "%s", _("hugetlb"));
						}
						if (shmdsp->shm_perm.mode & SHM_NORESERVE) {
							if (comma)
								arg[offt++] = ',';
							offt += sprintf(arg + offt, "%s", _("noreserve"));
						}
						rc = scols_line_set_data(ln, n, arg);
					}
					break;
				case COL_ATTACH:
					if (shmdsp->shm_atim != 0) {
						rc = scols_line_set_data(ln, n,
							time = make_time(ctl->time_mode,
								  (time_t)shmdsp->shm_atim));
						free(time);
					}
					break;
				case COL_DETACH:
					if (shmdsp->shm_dtim != 0) {
						rc = scols_line_set_data(ln, n,
							time = make_time(ctl->time_mode,
								  (time_t)shmdsp->shm_dtim));
						free(time);
					}
					break;
				case COL_CPID:
					xasprintf(&arg, "%u", shmdsp->shm_cprid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_LPID:
					xasprintf(&arg, "%u", shmdsp->shm_lprid);
					rc = scols_line_set_data(ln, n, arg);
					break;
				case COL_COMMAND:
					arg = proc_get_command(shmdsp->shm_cprid);
					rc = scols_line_set_data(ln, n, arg);
					break;
			}
			if (rc != 0)
				err(EXIT_FAILURE, _("failed to set data"));
			++n;
			free(arg);
			arg = NULL;
		}
		if (id > -1)
			break;
	}
	ipc_shm_free_info(shmds);
}

static void do_shm_global(struct libscols_table *tb)
{
	struct shm_data *shmds, *shmdsp;
	uint64_t nsegs = 0, sum_segsz = 0;
	struct ipc_limits lim;

	ipc_shm_get_limits(&lim);

	if (ipc_shm_get_info(-1, &shmds) > 0) {
		for (shmdsp = shmds; shmdsp->next != NULL; shmdsp = shmdsp->next) {
			++nsegs;
			sum_segsz += shmdsp->shm_segsz;
		}
	}

	global_set_data(tb, "SHMMNI", _("Shared memory segments"), nsegs, lim.shmmni);
	global_set_data(tb, "SHMALL", _("Shared memory pages"), sum_segsz / getpagesize(), lim.shmall);

	ipc_shm_free_info(shmds);
}

int main(int argc, char *argv[])
{
	int opt, msg = 0, sem = 0, shm = 0, opt_o = 0, id = -1;
	int show_time = 0, show_creat = 0, global = 0;
	size_t i;
	struct lsipc_control *ctl = xcalloc(1, sizeof(struct lsipc_control));
	static struct libscols_table *tb;
	char *opts = NULL;

	/* long only options. */
	enum {
		OPT_NOTRUNC = CHAR_MAX + 1,
		OPT_NOHEAD,
		OPT_TIME_FMT,
		OPT_COLON,
	};

	static const struct option longopts[] = {
		{ "bytes",          no_argument,        0, 'b' },
		{ "colon-separate", no_argument,	0, OPT_COLON },
		{ "creator",        no_argument,	0, 'c' },
		{ "export",         no_argument,	0, 'e' },
		{ "global",         no_argument,	0, 'g' },
		{ "help",           no_argument,	0, 'h' },
		{ "id",             required_argument,	0, 'i' },
		{ "json",           no_argument,	0, 'J' },
		{ "newline",        no_argument,	0, 'n' },
		{ "noheadings",     no_argument,	0, OPT_NOHEAD },
		{ "notruncate",     no_argument,	0, OPT_NOTRUNC },
		{ "numeric-perms",  no_argument,	0, 'P' },
		{ "output",         required_argument,	0, 'o' },
		{ "pid",            no_argument,	0, 'p' },
		{ "print0",         no_argument,	0, 'z' },
		{ "queues",         no_argument,	0, 'q' },
		{ "raw",            no_argument,	0, 'r' },
		{ "semaphores",     no_argument,	0, 's' },
		{ "shmems",         no_argument,	0, 'm' },
		{ "time",           no_argument,	0, 't' },
		{ "time-format",    required_argument,	0, OPT_TIME_FMT },
		{ "version",        no_argument,	0, 'V' },
		{NULL, 0, NULL, 0}
	};

	static const ul_excl_t excl[] = {	/* rows and cols in ASCII order */
		{ 'J', 'e', 'n', 'r', 'z', OPT_COLON },
		{ 'c', 'g', 'i', 't' },
		{ 'c', 'i', 'o', 't' },
		{ 'm', 'q', 's' },
		{ 0 }
	};
	int excl_st[ARRAY_SIZE(excl)] = UL_EXCL_STATUS_INIT;

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	ctl->time_mode = TIME_SHORT;

	while ((opt = getopt_long(argc, argv, "bceghi:Jmno:PqrstuVz", longopts, NULL)) != -1) {

		err_exclusive_options(opt, longopts, excl, excl_st);

		switch (opt) {
			case 'b':
				ctl->bytes = 1;
				break;
			case 'i':
				id = atoi (optarg);
				outmode = OUT_PRETTY;
				break;
			case OPT_COLON:
				outmode = OUT_COLON;
				break;
			case 'e':
				outmode = OUT_EXPORT;
				break;
			case 'r':
				outmode = OUT_RAW;
				break;
			case 'o':
				if (optarg) {
					if (*optarg == '=')
						optarg++;
					opts = xstrdup(optarg);
				}
				opt_o = 1;
				break;
			case 'g':
				global = 1;
				LOWER = COLDESC_IDX_SUM_FIRST;
				UPPER = COLDESC_IDX_SUM_LAST;
				break;
			case 'q':
				msg = 1;
				LOWER = COLDESC_IDX_MSG_FIRST;
				UPPER = COLDESC_IDX_MSG_LAST;
				break;
			case 'm':
				shm = 1;
				LOWER = COLDESC_IDX_SHM_FIRST;
				UPPER = COLDESC_IDX_SHM_LAST;
				break;
			case 'n':
				outmode = OUT_NEWLINE;
				break;
			case 'P':
				ctl->numperms = 1;
				break;
			case 's':
				sem = 1;
				LOWER = COLDESC_IDX_SEM_FIRST;
				UPPER = COLDESC_IDX_SEM_LAST;
				break;
			case OPT_NOTRUNC:
				ctl->notrunc = 1;
				break;
			case OPT_NOHEAD:
				ctl->noheadings = 1;
				break;
			case OPT_TIME_FMT:
				ctl->time_mode = parse_time_mode(optarg);
				break;
			case 'J':
				ctl->json = 1;
				break;
			case 't':
				show_time = 1;
				break;
			case 'c':
				show_creat = 1;
				break;
			case 'h':
				usage(stdout);
			case 'V':
				printf(UTIL_LINUX_VERSION);
				return EXIT_SUCCESS;
			case 'z':
				outmode = OUT_NUL;
				break;
			default:
				usage(stderr);
		}
	}

	if (msg + shm + sem != 1 && !global)
		errx (EXIT_FAILURE,
		      _("One of --shmems, --queues or --semaphores must be specified"));
	if (global && msg + shm + sem == 0)
		msg = shm = sem = 1;

	if (global && !opt_o) {
		add_column(columns, ncolumns++, COL_RESOURCE);
		add_column(columns, ncolumns++, COL_DESC);
		add_column(columns, ncolumns++, COL_USED);
		add_column(columns, ncolumns++, COL_LIMIT);
	} else if (opt_o) {
		ncolumns = string_to_idarray(opts,
				columns, ARRAY_SIZE(columns),
				column_name_to_id);
		if (ncolumns < 0)
			return EXIT_FAILURE;
	} else {
		if (outmode == OUT_PRETTY) {
			/* all columns for lsipc --<RESOURCE> --id <ID> */
			for (ncolumns = 0, i = 0; i < ARRAY_SIZE(coldescs); i++)
				 columns[ncolumns++] = i;
		} else {
			/* default columns */
			add_column(columns, ncolumns++, COL_KEY);
			add_column(columns, ncolumns++, COL_ID);
			add_column(columns, ncolumns++, COL_PERMS);
			add_column(columns, ncolumns++, COL_OWNER);

			if (show_creat) {
				add_column(columns, ncolumns++, COL_CUID);
				add_column(columns, ncolumns++, COL_CGID);
				add_column(columns, ncolumns++, COL_UID);
				add_column(columns, ncolumns++, COL_GID);
			}

			if (msg) {
				add_column(columns, ncolumns++, COL_USEDBYTES);
				add_column(columns, ncolumns++, COL_MSGS);

				if (show_time) {
					add_column(columns, ncolumns++, COL_SEND);
					add_column(columns, ncolumns++, COL_RECV);
					add_column(columns, ncolumns++, COL_CTIME);
				}

				add_column(columns, ncolumns++, COL_LSPID);
				add_column(columns, ncolumns++, COL_LRPID);
			}
			else if (shm) {
				add_column(columns, ncolumns++, COL_SIZE);
				add_column(columns, ncolumns++, COL_NATTCH);
				add_column(columns, ncolumns++, COL_STATUS);

				if (show_time) {
					add_column(columns, ncolumns++, COL_ATTACH);
					add_column(columns, ncolumns++, COL_DETACH);
				}
				add_column(columns, ncolumns++, COL_CTIME);

				add_column(columns, ncolumns++, COL_CPID);
				add_column(columns, ncolumns++, COL_LPID);
				add_column(columns, ncolumns++, COL_COMMAND);
			}
			else if (sem) {
				add_column(columns, ncolumns++, COL_NSEMS);

				if (show_time) {
					add_column(columns, ncolumns++, COL_OTIME);
					add_column(columns, ncolumns++, COL_CTIME);
				}
			}
		}
	}

	tb = setup_table(ctl);
	if (!tb)
		return EXIT_FAILURE;

	if (msg) {
		if (global)
			do_msg_global(tb);
		else
			do_msg(id, ctl, tb);
	}
	if (shm) {
		if (global)
			do_shm_global(tb);
		else
			do_shm(id, ctl, tb);
	}
	if (sem) {
		if (global)
			do_sem_global(tb);
		else
			do_sem(id, ctl, tb);
	}

	print_table(tb);

	scols_unref_table(tb);
	free(ctl);

	return EXIT_SUCCESS;
}

