/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */

/*
 * fsslower  Trace file system operations slower than a threshold.
 *
 * Copyright (c) 2020 Wenbo Zhang
 * Copyright (c) 2021 Hengqi Chen
 *
 * Based on xfsslower(8) from BCC by Brendan Gregg & Dina Goldshtein.
 * 9-Mar-2020   Wenbo Zhang   Created this.
 * 27-May-2021  Hengqi Chen   Migrated to fsslower.
 */
#include <argp.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include "fsslower.h"
#include "fsslower.skel.h"
#include "trace_helpers.h"

#define PERF_BUFFER_PAGES	64
#define PERF_POLL_TIMEOUT_MS	100

#define warn(...) fprintf(stderr, __VA_ARGS__)

enum fs_type {
	NONE,
	BTRFS,
	EXT4,
	NFS,
	XFS,
};

static struct fs_config {
	const char *fs;
	const char *op_funcs[MAX_OP];
} fs_configs[] = {
	[BTRFS] = { "btrfs", {
		[READ] = "btrfs_file_read_iter",
		[WRITE] = "btrfs_file_write_iter",
		[OPEN] = "btrfs_file_open",
		[FSYNC] = "btrfs_sync_file",
	}},
	[EXT4] = { "ext4", {
		[READ] = "ext4_file_read_iter",
		[WRITE] = "ext4_file_write_iter",
		[OPEN] = "ext4_file_open",
		[FSYNC] = "ext4_sync_file",
	}},
	[NFS] = { "nfs", {
		[READ] = "nfs_file_read",
		[WRITE] = "nfs_file_write",
		[OPEN] = "nfs_file_open",
		[FSYNC] = "nfs_file_fsync",
	}},
	[XFS] = { "xfs", {
		[READ] = "xfs_file_read_iter",
		[WRITE] = "xfs_file_write_iter",
		[OPEN] = "xfs_file_open",
		[FSYNC] = "xfs_file_fsync",
	}},
};

static char file_op[] = {
	[READ] = 'R',
	[WRITE] = 'W',
	[OPEN] = 'O',
	[FSYNC] = 'F',
};

static volatile sig_atomic_t exiting;

/* options */
static enum fs_type fs_type = NONE;
static pid_t target_pid = 0;
static time_t duration = 0;
static __u64 min_lat_ms = 10;
static bool csv = false;
static bool verbose = false;

const char *argp_program_version = "fsslower 0.1";
const char *argp_program_bug_address =
	"https://github.com/iovisor/bcc/tree/master/libbpf-tools";
const char argp_program_doc[] =
"Trace file system operations slower than a threshold.\n"
"\n"
"Usage: fsslower [-h] [-t FS] [-p PID] [-m MIN] [-d DURATION] [-c]\n"
"\n"
"EXAMPLES:\n"
"    fsslower -t ext4             # trace ext4 operations slower than 10 ms\n"
"    fsslower -t nfs -p 1216      # trace nfs operations with PID 1216 only\n"
"    fsslower -t xfs -c -d 1      # trace xfs operations for 1s with csv output\n";

static const struct argp_option opts[] = {
	{ "csv", 'c', NULL, 0, "Output as csv" },
	{ "duration", 'd', "DURATION", 0, "Total duration of trace in seconds" },
	{ "pid", 'p', "PID", 0, "Process ID to trace" },
	{ "min", 'm', "MIN", 0, "Min latency to trace, in ms (default 10)" },
	{ "type", 't', "Filesystem", 0, "Which filesystem to trace, [btrfs/ext4/nfs/xfs]" },
	{ "verbose", 'v', NULL, 0, "Verbose debug output" },
	{ NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help" },
	{},
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 'v':
		verbose = true;
		break;
	case 'c':
		csv = true;
		break;
	case 'd':
		errno = 0;
		duration = strtol(arg, NULL, 10);
		if (errno || duration <= 0) {
			warn("invalid DURATION: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'm':
		errno = 0;
		min_lat_ms = strtoll(arg, NULL, 10);
		if (errno || min_lat_ms < 0) {
			warn("invalid latency (in ms): %s\n", arg);
		}
		break;
	case 't':
		if (!strcmp(arg, "btrfs")) {
			fs_type = BTRFS;
		} else if (!strcmp(arg, "ext4")) {
			fs_type = EXT4;
		} else if (!strcmp(arg, "nfs")) {
			fs_type = NFS;
		} else if (!strcmp(arg, "xfs")) {
			fs_type = XFS;
		} else {
			warn("invalid filesystem\n");
			argp_usage(state);
		}
		break;
	case 'p':
		errno = 0;
		target_pid = strtol(arg, NULL, 10);
		if (errno || target_pid <= 0) {
			warn("invalid PID: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'h':
		argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static void alias_parse(char *prog)
{
	char *name = basename(prog);

	if (!strcmp(name, "btrfsslower")) {
		fs_type = BTRFS;
	} else if (!strcmp(name, "ext4slower")) {
		fs_type = EXT4;
	} else if (!strcmp(name, "nfsslower")) {
		fs_type = NFS;
	} else if (!strcmp(name, "xfsslower")) {
		fs_type = XFS;
	}
}

static int libbpf_print_fn(enum libbpf_print_level level,
			   const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sig_handler(int sig)
{
	exiting = 1;
}

static bool check_fentry()
{
	int i;
	const char *fn_name, *module;
	bool support_fentry = true;

	for (i = 0; i < MAX_OP; i++) {
		fn_name = fs_configs[fs_type].op_funcs[i];
		module = fs_configs[fs_type].fs;
		if (fn_name && !fentry_exists(fn_name, NULL)
		    && !fentry_exists(fn_name, module)) {
			support_fentry = false;
			break;
		}
	}
	return support_fentry;
}

static int fentry_set_attach_target(struct fsslower_bpf *obj)
{
	struct fs_config *cfg = &fs_configs[fs_type];
	int err = 0;

	err = err ?: bpf_program__set_attach_target(obj->progs.file_read_fentry, 0, cfg->op_funcs[READ]);
	err = err ?: bpf_program__set_attach_target(obj->progs.file_read_fexit, 0, cfg->op_funcs[READ]);
	err = err ?: bpf_program__set_attach_target(obj->progs.file_write_fentry, 0, cfg->op_funcs[WRITE]);
	err = err ?: bpf_program__set_attach_target(obj->progs.file_write_fexit, 0, cfg->op_funcs[WRITE]);
	err = err ?: bpf_program__set_attach_target(obj->progs.file_open_fentry, 0, cfg->op_funcs[OPEN]);
	err = err ?: bpf_program__set_attach_target(obj->progs.file_open_fexit, 0, cfg->op_funcs[OPEN]);
	err = err ?: bpf_program__set_attach_target(obj->progs.file_sync_fentry, 0, cfg->op_funcs[FSYNC]);
	err = err ?: bpf_program__set_attach_target(obj->progs.file_sync_fexit, 0, cfg->op_funcs[FSYNC]);
	return err;
}

static void disable_fentry(struct fsslower_bpf *obj)
{
	bpf_program__set_autoload(obj->progs.file_read_fentry, false);
	bpf_program__set_autoload(obj->progs.file_read_fexit, false);
	bpf_program__set_autoload(obj->progs.file_write_fentry, false);
	bpf_program__set_autoload(obj->progs.file_write_fexit, false);
	bpf_program__set_autoload(obj->progs.file_open_fentry, false);
	bpf_program__set_autoload(obj->progs.file_open_fexit, false);
	bpf_program__set_autoload(obj->progs.file_sync_fentry, false);
	bpf_program__set_autoload(obj->progs.file_sync_fexit, false);
}

static void disable_kprobes(struct fsslower_bpf *obj)
{
	bpf_program__set_autoload(obj->progs.file_read_entry, false);
	bpf_program__set_autoload(obj->progs.file_read_exit, false);
	bpf_program__set_autoload(obj->progs.file_write_entry, false);
	bpf_program__set_autoload(obj->progs.file_write_exit, false);
	bpf_program__set_autoload(obj->progs.file_open_entry, false);
	bpf_program__set_autoload(obj->progs.file_open_exit, false);
	bpf_program__set_autoload(obj->progs.file_sync_entry, false);
	bpf_program__set_autoload(obj->progs.file_sync_exit, false);
}

static int attach_kprobes(struct fsslower_bpf *obj)
{
	long err = 0;
	struct fs_config *cfg = &fs_configs[fs_type];

	/* READ */
	obj->links.file_read_entry = bpf_program__attach_kprobe(obj->progs.file_read_entry, false, cfg->op_funcs[READ]);
	err = libbpf_get_error(obj->links.file_read_entry);
	if (err)
		goto errout;
	obj->links.file_read_exit = bpf_program__attach_kprobe(obj->progs.file_read_exit, true, cfg->op_funcs[READ]);
	err = libbpf_get_error(obj->links.file_read_exit);
	if (err)
		goto errout;
	/* WRITE */
	obj->links.file_write_entry = bpf_program__attach_kprobe(obj->progs.file_write_entry, false, cfg->op_funcs[WRITE]);
	err = libbpf_get_error(obj->links.file_write_entry);
	if (err)
		goto errout;
	obj->links.file_write_exit = bpf_program__attach_kprobe(obj->progs.file_write_exit, true, cfg->op_funcs[WRITE]);
	err = libbpf_get_error(obj->links.file_write_exit);
	if (err)
		goto errout;
	/* OPEN */
	obj->links.file_open_entry = bpf_program__attach_kprobe(obj->progs.file_open_entry, false, cfg->op_funcs[OPEN]);
	err = libbpf_get_error(obj->links.file_open_entry);
	if (err)
		goto errout;
	obj->links.file_open_exit = bpf_program__attach_kprobe(obj->progs.file_open_exit, true, cfg->op_funcs[OPEN]);
	err = libbpf_get_error(obj->links.file_open_exit);
	if (err)
		goto errout;
	/* FSYNC */
	obj->links.file_sync_entry = bpf_program__attach_kprobe(obj->progs.file_sync_entry, false, cfg->op_funcs[FSYNC]);
	err = libbpf_get_error(obj->links.file_sync_entry);
	if (err)
		goto errout;
	obj->links.file_sync_exit = bpf_program__attach_kprobe(obj->progs.file_sync_exit, true, cfg->op_funcs[FSYNC]);
	err = libbpf_get_error(obj->links.file_sync_exit);
	if (err)
		goto errout;
	return 0;

errout:
	warn("failed to attach kprobe: %ld\n", err);
	return err;
}

static void print_headers()
{
	const char *fs = fs_configs[fs_type].fs;

	if (csv) {
		printf("ENDTIME_ns,TASK,PID,TYPE,BYTES,OFFSET_b,LATENCY_us,FILE\n");
		return;
	}

	if (min_lat_ms)
		printf("Tracing %s operations slower than %llu ms", fs, min_lat_ms);
	else
		printf("Tracing %s operations", fs);

	if (duration)
		printf(" for %ld secs.\n", duration);
	else
		printf("... Hit Ctrl-C to end.\n");

	printf("%-8s %-16s %-7s %1s %-7s %-8s %7s %s\n",
	       "TIME", "COMM", "PID", "T", "BYTES", "OFF_KB", "LAT(ms)", "FILENAME");
}

static void handle_event(void *ctx, int cpu, void *data, __u32 data_sz)
{
	const struct event *e = data;
	struct tm *tm;
	char ts[32];
	time_t t;

	if (csv) {
		printf("%lld,%s,%d,%c,", e->end_ns, e->task, e->pid, file_op[e->op]);
		if (e->size == LLONG_MAX)
			printf("LL_MAX,");
		else
			printf("%ld,", e->size);
		printf("%lld,%lld,%s\n", e->offset, e->delta_us, e->file);
		return;
	}

	time(&t);
	tm = localtime(&t);
	strftime(ts, sizeof(ts), "%H:%M:%S", tm);

	printf("%-8s %-16s %-7d %c ", ts, e->task, e->pid, file_op[e->op]);
	if (e->size == LLONG_MAX)
		printf("%-7s ", "LL_MAX");
	else
		printf("%-7ld ", e->size);
	printf("%-8lld %7.2f %s\n", e->offset / 1024, (double)e->delta_us / 1000, e->file);
}

static void handle_lost_events(void *ctx, int cpu, __u64 lost_cnt)
{
	warn("lost %llu events on CPU #%d\n", lost_cnt, cpu);
}

int main(int argc, char **argv)
{
	static const struct argp argp = {
		.options = opts,
		.parser = parse_arg,
		.doc = argp_program_doc,
	};
	struct perf_buffer_opts pb_opts;
	struct perf_buffer *pb = NULL;
	struct fsslower_bpf *skel;
	__u64 time_end = 0;
	int err;
	bool support_fentry;

	alias_parse(argv[0]);
	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return err;
	if (fs_type == NONE) {
		warn("filesystem must be specified using -t option.\n");
		return 1;
	}

	libbpf_set_print(libbpf_print_fn);

	err = bump_memlock_rlimit();
	if (err) {
		warn("failed to increase rlimit: %d\n", err);
		return 1;
	}

	skel = fsslower_bpf__open();
	if (!skel) {
		warn("failed to open BPF object\n");
		return 1;
	}

	skel->rodata->target_pid = target_pid;
	skel->rodata->min_lat_ns = min_lat_ms * 1000 * 1000;

	/*
	 * before load
	 * if fentry is supported, we set attach target and disable kprobes
	 * otherwise, we disable fentry and attach kprobes after loading
	 */
	support_fentry = check_fentry();
	if (support_fentry) {
		err = fentry_set_attach_target(skel);
		if (err) {
			warn("failed to set attach target: %d\n", err);
			goto cleanup;
		}
		disable_kprobes(skel);
	} else {
		disable_fentry(skel);
	}

	err = fsslower_bpf__load(skel);
	if (err) {
		warn("failed to load BPF object: %d\n", err);
		goto cleanup;
	}

	/*
	 * after load
	 * if fentry is supported, let libbpf do auto load
	 * otherwise, we attach to kprobes manually
	 */
	err = support_fentry ? fsslower_bpf__attach(skel) : attach_kprobes(skel);
	if (err) {
		warn("failed to attach BPF programs: %d\n", err);
		goto cleanup;
	}

	signal(SIGINT, sig_handler);

	pb_opts.sample_cb = handle_event;
	pb_opts.lost_cb = handle_lost_events;
	pb = perf_buffer__new(bpf_map__fd(skel->maps.events), PERF_BUFFER_PAGES,
			      &pb_opts);
	err = libbpf_get_error(pb);
	if (err) {
		pb = NULL;
		warn("failed to open perf buffer: %d\n", err);
		goto cleanup;
	}

	print_headers();

	if (duration)
		time_end = get_ktime_ns() + duration * NSEC_PER_SEC;

	while (1) {
		if ((err = perf_buffer__poll(pb, PERF_POLL_TIMEOUT_MS)) < 0)
			break;
		if (duration && get_ktime_ns() > time_end)
			goto cleanup;
	}
	warn("failed with polling perf buffer: %d\n", err);

cleanup:
	perf_buffer__free(pb);
	fsslower_bpf__destroy(skel);

	return err != 0;
}
