/**
 * @file	opal_errd.c
 * @brief	Daemon to read/parse OPAL error/events
 *
 * Copyright (C) 2014 IBM Corporation
 */

/*
 * This file supports
 *   1. Reading OPAL platform logs from sysfs
 *   2. Writing OPAL platform logs to individual files under /var/log/opal-elog
 *   3. ACK platform log
 *   4. Parsing required fields from log and write to syslog
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <signal.h>
#include <sys/stat.h>
#include <limits.h>
#include <poll.h>
#include <dirent.h>
#include <libgen.h>
#include <sys/inotify.h>
#include <assert.h>
#include <endian.h>
#include <inttypes.h>
#include <time.h>
#include <libudev.h>
#include "opal-elog-parse/opal-event-data.h"
#define INOTIFY_FD	0
#define UDEV_FD		1
#define POLL_TIMEOUT	1000 /* In milliseconds */

#define DEFAULT_SYSFS_PATH		"/sys"
#define DEFAULT_OUTPUT_DIR		"/var/log/opal-elog"
#define DEFAULT_EXTRACT_DUMP_CMD	"/usr/sbin/extract_opal_dump"
#define DEFAULT_EXTRACT_DUMP_FNAME "extract_opal_dump"

/**
 * Length of elog ID string (including the null)
 * eg: 0x12345678
 */
#define ELOG_STR_SIZE	11
/**
 * ELOG retention policy
 *
 * Retain logs upto 30 days with max 1000 logs.
 */
#define DEFAULT_MAX_ELOGS		1000
#define DEFAULT_MAX_DAYS		30

/*
 * As per PEL v6 (defined in PAPR spec) fixed offset for
 * error log information.
 */
#define ELOG_SRC_SIZE		8

#define ELOG_DATE_OFFSET	0x8
#define ELOG_TIME_OFFSET	0xc
#define ELOG_ID_OFFESET		0x2c
#define ELOG_SEVERITY_OFFSET	0x3a
#define ELOG_SUBSYSTEM_OFFSET	0x38
#define ELOG_ACTION_OFFSET	0x42
#define ELOG_SRC_OFFSET		0x78
#define ELOG_MIN_READ_OFFSET	ELOG_SRC_OFFSET + ELOG_SRC_SIZE

/* Severity of the log */
#define OPAL_INFORMATION_LOG	0x00
#define OPAL_RECOVERABLE_LOG	0x10
#define OPAL_PREDICTIVE_LOG	0x20
#define OPAL_UNRECOVERABLE_LOG	0x40
#define OPAL_CRITICAL_LOG	0x50
#define OPAL_DIAGNOSTICS_LOG	0x60
#define OPAL_SYMPTOM_LOG	0x70

#define ELOG_ACTION_FLAG_SERVICE	0x8000
#define ELOG_ACTION_FLAG_CALL_HOME	0x0800

volatile int terminate;

/* Safe to ignore sig, this only gets called on SIGTERM */
static void term_handler(int sig)
{
	terminate = 1;
}

/* may move this into header to avoid code duplication */
static int file_filter(const struct dirent *d)
{
	struct stat sbuf;

	if (d->d_type == DT_DIR)
		return 0;
	if (d->d_type == DT_REG)
		return 1;
	if (stat(d->d_name,&sbuf))
		return 0;
	if (S_ISDIR(sbuf.st_mode))
		return 0;
	if (d->d_type == DT_UNKNOWN)
		return 1;

	return 0;
}

/* Your job to free the return value */
static char *find_opal_errd_dir(void)
{
	struct stat sb;
	ssize_t r;
	char *errd_path;
	char *path_end;
	if (lstat("/proc/self/exe", &sb) == -1)
		return NULL;

	errd_path = malloc(sb.st_size + 1);
	if (!errd_path)
		return NULL;

	r = readlink ("/proc/self/exe", errd_path, sb.st_size);
	if (r <= 0 || r > sb.st_size)
		return NULL;

	/* Just interested in the path, trim fname  */
	path_end = strrchr (errd_path, '/');
	if (path_end == NULL)
		return NULL;
	/* + 1 to ensure the trailing / stays in */
	*(path_end + 1) = '\0';
	return errd_path;
}

/* Your job to free the returned path */
static int find_extract_opal_dump_cmd(char **r_dump_path)
{
	char *errd_path;
	char *dump_path;
	*r_dump_path = NULL;
	/* Check default location */
	if (access(DEFAULT_EXTRACT_DUMP_CMD, X_OK) == 0) {
		/* Exists */
		*r_dump_path = strdup(DEFAULT_EXTRACT_DUMP_CMD);
		return 0;
	}

	errd_path = find_opal_errd_dir();
	if (!errd_path)
		return -1;

	/* Look in whereever errd was executed from */
	dump_path = malloc(strlen(errd_path) + strlen(DEFAULT_EXTRACT_DUMP_FNAME) + 1);
	if (!dump_path) {
		free(errd_path);
		return -1;
	}

	strcpy(dump_path, errd_path);
	strcat(dump_path, DEFAULT_EXTRACT_DUMP_FNAME);
	free(errd_path);
	if (access(dump_path, X_OK) == 0) {
		*r_dump_path = dump_path;
		return 0;
	}

	free(dump_path);
	return -1;
}

static int rotate_logs(const char *elog_dir, int max_logs, int max_age)
{
	int i;
	int nfiles;
	int ret = 0;
	int old = 1;
	int trim = 1;
	struct dirent **filelist;
	char *elog_name;
	char *elog_date;
	int max = max_age * 24 * 60 * 60;
	int now = (int) time(NULL);

	/* Retrieve file list */
	chdir(elog_dir);
	nfiles = scandir(elog_dir, &filelist, file_filter, alphasort);
	if (nfiles < 0)
		return -1;

	for (i = 0; i < nfiles; i++) {
		if(!old && !trim){
			free(filelist[i]);
			continue;
		}

		elog_name = strdup(filelist[i]->d_name);
		if (!elog_name) {
			syslog(LOG_NOTICE, "Failed to allocate memory\n");
			free(filelist[i]);
			continue;
		}

		/* Names are ordered 'oldest first' from scandir */
		if(i >= (nfiles - max_logs))
			trim = 0;

		/* Extract date from filename */
		elog_date = strtok(elog_name, "-");
		if(!elog_date){
			syslog(LOG_NOTICE, "Failed to read file\n");
			free(elog_name);
			free(filelist[i]);
			continue;
		}

		errno = 0;
		int date = strtol(elog_date, NULL, 10);
		if(errno || !date){
			syslog(LOG_NOTICE, "Failed to parse file date\n");
			free(elog_name);
			free(filelist[i]);
			continue;
		}
		if(now - date < max)
			old = 0;

		if(old || trim) {
			ret = remove(filelist[i]->d_name);
			if(ret)
				syslog(LOG_NOTICE, "Error removing %s\n",
				       filelist[i]->d_name);
		}

		free(filelist[i]);
		free(elog_name);
	}
	free(filelist);

	return ret;
}

/* Parse required fields from error log */
static int parse_log(char *buffer, size_t bufsz)
{
	uint32_t logid;
	char src[ELOG_SRC_SIZE+1];
	uint8_t severity;
	uint8_t subsysid;
	uint16_t action;
	const char *parse;
	char *parse_action = "NONE";
	const char *failingsubsys = "Not Applicable";

	if (bufsz < ELOG_MIN_READ_OFFSET) {
		syslog(LOG_NOTICE, "Insufficent data, cannot parse elog.\n");
		return -1;
	}

	logid = be32toh(*(uint32_t*)(buffer + ELOG_ID_OFFESET));

	memcpy(src, (buffer + ELOG_SRC_OFFSET), ELOG_SRC_SIZE);
	src[ELOG_SRC_SIZE] = '\0';

	subsysid = buffer[ELOG_SUBSYSTEM_OFFSET];
	severity = buffer[ELOG_SEVERITY_OFFSET];

	/* Every category has a generic entry at 0x?0 */
	parse = get_severity_desc(severity & 0xF0);

	action = be16toh(*(uint16_t *)(buffer + ELOG_ACTION_OFFSET));
	if ((action & ELOG_ACTION_FLAG_SERVICE) &&
	    (action & ELOG_ACTION_FLAG_CALL_HOME))
		parse_action = "Service action and call home required";
	else if ((action & ELOG_ACTION_FLAG_SERVICE))
		parse_action = "Service action required";
	else
		parse_action = "No service action required";

	/* Every category has a generic entry at 0x?0 */
	failingsubsys = get_subsystem_name(subsysid  & 0xF0);

	syslog(LOG_NOTICE, "LID[%x]::SRC[%s]::%s::%s::%s\n",
	       logid, src, failingsubsys, parse, parse_action);

	if ((action & ELOG_ACTION_FLAG_SERVICE) &&
	    !(action & ELOG_ACTION_FLAG_CALL_HOME))
		syslog(LOG_NOTICE, "Run \'opal-elog-parse -d 0x%x\' "
		       "for the details.\n", logid);

	return 0;
}

/**
 * Check platform dump
 */
static void check_platform_dump(const char *extract_opal_dump_cmd,
		const char *sysfs_path, const char *max_dump)
{
	if (!extract_opal_dump_cmd || !sysfs_path)
		return;
	int rc;
	int length = PATH_MAX + strlen(extract_opal_dump_cmd);
	char dump_cmd[length];

	if (access(extract_opal_dump_cmd, X_OK) != 0) {
		syslog(LOG_NOTICE, "The command \"%s\" is not executable.\n",
				extract_opal_dump_cmd);
		return;
	}

	rc = snprintf(dump_cmd, length, "%s -s %s",
			extract_opal_dump_cmd, sysfs_path);
	if (rc >= length) {
		syslog(LOG_NOTICE, "Failed to execute platform dump extractor"
		       " (%s) as command options were truncated.\n", dump_cmd);
		return;
	}

	if (max_dump)	/* Append -m flag */
		rc += snprintf(dump_cmd + rc, length - rc, " -m %s", max_dump);

	if (rc >= length) {
		syslog(LOG_NOTICE, "Failed to execute platform dump extractor"
		       " (%s) as command options were truncated.\n", dump_cmd);
		return;
	}

	rc = system(dump_cmd);
	if (rc) {
		syslog(LOG_NOTICE, "Failed to execute platform dump "
		       "extractor (%s).\n", dump_cmd);
		return;
	}
}

static int ack_elog(const char *elog_path)
{
	char ack_file[PATH_MAX];
	int fd;
	int rc;

	rc = snprintf(ack_file, sizeof(ack_file), "%s/acknowledge", elog_path);
	if (rc >= PATH_MAX) {
		syslog(LOG_ERR, "Path to elog ack file is too big\n");
		return -1;
	}

	fd = open(ack_file, O_WRONLY);

	if (fd == -1) {
		syslog(LOG_ERR, "Failed to acknowledge elog: %s"
		       " (%d:%s)\n",
		       ack_file, errno, strerror(errno));
		return -1;
	}

	rc = write(fd, "ack\n", 4);
	if (rc != 4) {
		syslog(LOG_ERR, "Failed to acknowledge elog: %s"
		       " (%d:%s)\n",
		       ack_file, errno, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

static int process_elog(const char *elog_path, const char *output)
{
	int in_fd = -1;
	int out_fd = -1;
	int dir_fd = -1;
	char elog_raw_path[PATH_MAX];
	char *buf;
	char *name;
	size_t bufsz;
	struct stat sbuf;
	int ret = -1;
	ssize_t sz = 0;
	ssize_t readsz = 0;
	int rc;
	char *output_dir = strdup(output);
	char output_file[PATH_MAX];

	rc = snprintf(elog_raw_path, sizeof(elog_raw_path),
		      "%s/raw", elog_path);
	if (rc >= PATH_MAX) {
		syslog(LOG_ERR, "Path to elog file is too big\n");
		return -1;
	}

	if (stat(elog_raw_path, &sbuf) == -1)
		return -1;

	bufsz = sbuf.st_size;
	buf = (char*)malloc(bufsz);
	if (!buf) {
		syslog(LOG_ERR, "Failed to allocate memory\n");
		return -1;
	}

	in_fd = open(elog_raw_path, O_RDONLY);
	if (in_fd == -1) {
		syslog(LOG_ERR, "Failed to open elog: %s (%d:%s)\n",
		       elog_raw_path, errno, strerror(errno));
		goto err;
	}

	do {
		readsz = read(in_fd, buf+sz, bufsz-sz);
		if (readsz == -1) {
			syslog(LOG_ERR, "Failed to read elog: %s (%d:%s)\n",
			       elog_raw_path, errno, strerror(errno));
			goto err;
		}

		sz += readsz;
	} while(sz != bufsz);

	/* Parse elog filename */
	name = basename(dirname(elog_raw_path));
	rc = snprintf(output_file, sizeof(output_file), "%s/%d-%s",
			output, (int)time(NULL), name);
	if (rc >= PATH_MAX) {
		syslog(LOG_ERR, "Path to elog output file is too big\n");
		return -1;
	}

	out_fd = open(output_file, O_WRONLY  | O_CREAT,
			S_IRUSR | S_IWUSR | S_IRGRP);

	if (out_fd == -1) {
		syslog(LOG_ERR, "Failed to create elog output file: %s (%d:%s)\n",
		       output_file, errno, strerror(errno));
		goto err;
	}

	sz = write(out_fd, buf, bufsz);
	if (sz != bufsz) {
		syslog(LOG_ERR, "Failed to write elog output file: %s (%d:%s)\n",
		       output_file, errno, strerror(errno));
		goto err;
	}

	rc = fsync(out_fd);
	if (rc == -1) {
		syslog(LOG_ERR, "Failed to sync elog output file: %s (%d:%s)\n",
		       output_file, errno, strerror(errno));
		goto err;
	}

	dir_fd = open(dirname(output_dir), O_RDONLY|O_DIRECTORY);
	rc = fsync(dir_fd);
	if (rc == -1) {
		syslog(LOG_ERR, "Failed to sync platform elog directory: %s"
		       " (%d:%s)\n", output_dir, errno, strerror(errno));
	}

	parse_log(buf, bufsz);

	ret = 0;
err:
	if (in_fd != -1)
		close(in_fd);
	if (out_fd != -1)
		close(out_fd);
	if (dir_fd != -1)
		close(dir_fd);
	free(output_dir);
	free(buf);
	return ret;
}

/* Read logs from opal sysfs interface */
static int find_and_read_elog_events(const char *elog_dir, const char *output_path)
{
	int rc = 0;
	struct dirent **namelist;
	struct dirent *dirent;
	char elog_path[PATH_MAX];
	int is_dir = 0;
	struct stat sbuf;
	int retval = 0;
	int n;
	int i;

	n = scandir(elog_dir, &namelist, NULL, alphasort);
	if (n < 0)
		return -1;

	for (i = 0; i < n; i++) {
		dirent = namelist[i];

		if (dirent->d_name[0] == '.') {
			free(namelist[i]);
			continue;
		}

		snprintf(elog_path, sizeof(elog_path), "%s/%s",
			 elog_dir, dirent->d_name);

		is_dir = 0;

		if (dirent->d_type == DT_DIR) {
			is_dir = 1;
		} else {
			/* Fall back to stat() */
			rc = stat(elog_path, &sbuf);
			if (S_ISDIR(sbuf.st_mode)) {
				is_dir = 1;
			}
		}

		if (is_dir) {
			rc = process_elog(elog_path, output_path);
			if (rc != 0 && retval == 0)
				retval = -1;
			if (rc == 0 && retval >= 0)
				retval++;
			ack_elog(elog_path);
		}

		free(namelist[i]);
	}

	free(namelist);

	return retval;
}

static char *validate_extract_opal_dump(const char *cmd)
{
	char *extract_opal_dump_cmd = NULL;
	/* User didn't specifiy an extract_opal_dump command */
	if (!cmd) {
		if (find_extract_opal_dump_cmd(&extract_opal_dump_cmd) != 0)
			syslog(LOG_WARNING, "Could not find an opal dump extractor tool\n");
	} else {
		if (access(cmd, X_OK) == 0) {
			extract_opal_dump_cmd = strdup(cmd);
			if (!extract_opal_dump_cmd)
				syslog(LOG_ERR, "Memory allocation error, extract_opal_dump "
						"will not be called\n");
		} else {
			syslog(LOG_WARNING, "Couldn't execute extract_opal_dump "
					"command: %s (%d, %s), dumps will not be extracted\n",
					cmd, errno, strerror(errno));
		}
	}

	return extract_opal_dump_cmd;
}

static int opal_init_udev(struct udev **r_udev,
			  struct udev_monitor **r_udev_mon, int *r_fd)
{
	struct udev *udev = NULL;
	struct udev_monitor *udev_mon = NULL;
	int fd = -1;
	int rc = -1;

	udev = udev_new();
	if (!udev) {
		syslog(LOG_ERR, "Error creating udev object");
		goto init_udev_exit;
	}

	udev_mon = udev_monitor_new_from_netlink(udev, "udev");
	if (!udev_mon) {
		syslog(LOG_ERR, "Error creating udev monitor object");
		goto init_udev_exit;
	}

	rc = udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "dump", NULL);
	if (rc < 0) {
		syslog(LOG_ERR, "Error (%d) adding udev match to dump", rc);
		goto init_udev_exit;
	}

	rc = udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "elog", NULL);
	if (rc < 0) {
		syslog(LOG_ERR, "Error (%d) adding udev match to elog", rc);
		goto init_udev_exit;
	}

	rc = udev_monitor_enable_receiving(udev_mon);
	if (rc < 0) {
		syslog(LOG_ERR, "Error (%d) enabling recieving on udev", rc);
		goto init_udev_exit;
	}

	fd = udev_monitor_get_fd(udev_mon);

init_udev_exit:
	if (rc != 0) {
		if (udev_mon)
			udev_monitor_unref(udev_mon);
		if (udev)
			udev_unref(udev);
		udev = NULL;
		udev_mon = NULL;
	}
	*r_udev = udev;
	*r_udev_mon = udev_mon;
	*r_fd = fd;
	return rc;
}

static void help(const char* argv0)
{
	fprintf(stderr, "%s help:\n\n", argv0);
	fprintf(stderr, "-e cmd  - path to extract_opal_dump (default %s)\n",
		DEFAULT_EXTRACT_DUMP_CMD);
	fprintf(stderr, "-o dir  - output log entries to directory (default %s)\n",
		DEFAULT_OUTPUT_DIR);
	fprintf(stderr, "-s dir  - path to sysfs (default %s)\n",
		DEFAULT_SYSFS_PATH);
	fprintf(stderr, "-D      - don't daemonize, just run once.\n");
	fprintf(stderr, "-w      - watch for new events (default when daemon)\n");
	fprintf(stderr, "-m max  - maximum number of dumps of a specific type"
			" to be saved\n");
	fprintf(stderr, "-n max  - maximum number of elogs to keep (default %d)\n",
			DEFAULT_MAX_ELOGS);
	fprintf(stderr, "-a days - maximum age in days of elogs to keep (default %d)\n",
			DEFAULT_MAX_DAYS);
	fprintf(stderr, "-h      - help (this message)\n");
}

int main(int argc, char *argv[])
{
	int rc = 0;
	int opt;
	char sysfs_path[PATH_MAX];
	char elog_path[PATH_MAX];
	char *extract_opal_dump_cmd = NULL;

	int log_options;

	struct udev *udev = NULL;
	struct udev_monitor *udev_mon = NULL;
	struct udev_device *udev_dev = NULL;
	struct pollfd fds[2];
	fds[INOTIFY_FD].fd = -1;
	char inotifybuf[sizeof(struct inotify_event) + NAME_MAX + 1];

	const char *devpath;
	char elog_str_name[ELOG_STR_SIZE];
	struct sigaction siga;

	int opt_daemon = 1;
	int opt_watch = 1;
	int opt_max_logs = DEFAULT_MAX_ELOGS;
	int opt_max_age = DEFAULT_MAX_DAYS;
	const char *opt_extract_opal_dump_cmd = NULL;
	const char *opt_max_dump = NULL;
	const char *opt_sysfs = DEFAULT_SYSFS_PATH;
	const char *opt_output_dir = DEFAULT_OUTPUT_DIR;

	while ((opt = getopt(argc, argv, "De:ho:s:m:wn:a:")) != -1) {
		switch (opt) {
		case 'D':
			opt_daemon = 0;
			opt_watch = 0;
			break;
		case 'w':
			opt_daemon = 0;
			opt_watch = 1;
			break;
		case 'o':
			opt_output_dir = optarg;
			break;
		case 'e':
			opt_extract_opal_dump_cmd = optarg;
			break;
		case 's':
			opt_sysfs = optarg;
			break;
		case 'm':
			opt_max_dump = optarg;
			break;
		case 'n':
			errno = 0;
			opt_max_logs = strtol(optarg,0,0);
			if(errno || opt_max_logs < 0){
				fprintf(stderr,"Invalid input for -n\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'a':
			errno = 0;
			opt_max_age = strtol(optarg,0,0);
			if(errno || opt_max_age < 0){
				fprintf(stderr,"Invalid input for -a\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'h':
			help(argv[0]);
			exit(EXIT_SUCCESS);
		default:
			help(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	/* syslog initialization */
	setlogmask(LOG_UPTO(LOG_NOTICE));
	log_options = LOG_CONS | LOG_PID | LOG_NDELAY;
	if (!opt_daemon)
		log_options |= LOG_PERROR;
	openlog("ELOG", log_options, LOG_LOCAL1);

	/* Do we have a valid extract_opal_dump?
	 * Confirm that what the user entered is valid
	 * If not, try to locate a valid extract_opal_dump binary
	 */
	extract_opal_dump_cmd = validate_extract_opal_dump(opt_extract_opal_dump_cmd);

	/* Use PATH_MAX but admit that it may be insufficient */
	rc = snprintf(sysfs_path, sizeof(sysfs_path), "%s/firmware/opal",
		      opt_sysfs);
	if (rc >= PATH_MAX) {
		syslog(LOG_ERR, "sysfs_path for opal dir is too big\n");
		rc = EXIT_FAILURE;
		goto exit;
	}

	/* elog log path */
	rc = snprintf(elog_path, sizeof(elog_path), "%s/elog", sysfs_path);
	if (rc >= PATH_MAX) {
		syslog(LOG_ERR, "sysfs_path for elogs too big\n");
		rc = EXIT_FAILURE;
		goto exit;
	}

	rc = access(sysfs_path, R_OK);
	if (rc != 0) {
		syslog(LOG_ERR, "Error accessing sysfs: %s (%d: %s)\n",
		       sysfs_path, errno, strerror(errno));
		goto exit;
	}

	rc = access(opt_output_dir, W_OK);
	if (rc != 0) {
		if (errno == ENOENT) {
			rc = mkdir(opt_output_dir,
				   S_IRGRP | S_IRUSR | S_IWGRP | S_IWUSR | S_IXUSR);
			if (rc != 0) {
				syslog(LOG_ERR, "Error creating output directory:"
						" %s (%d: %s)\n", opt_output_dir, errno,
						strerror(errno));
				goto exit;
			}
		} else {
			syslog(LOG_ERR, "Error accessing directory: %s (%d: %s)\n",
					opt_output_dir, errno, strerror(errno));
			goto exit;
		}
	}

	fds[INOTIFY_FD].fd = inotify_init();
	if (fds[INOTIFY_FD].fd == -1) {
		syslog(LOG_ERR, "Error setting up inotify (%d:%s)\n",
		       errno, strerror(errno));
		rc = EXIT_FAILURE;
		goto exit;
	}

	rc = inotify_add_watch(fds[INOTIFY_FD].fd, sysfs_path, IN_CREATE);
	if (rc == -1) {
		syslog(LOG_ERR, "Error adding inotify watch for %s (%d: %s)\n",
		       sysfs_path, errno, strerror(errno));
		goto exit;
	}

	rc = opal_init_udev(&udev, &udev_mon, &(fds[UDEV_FD].fd));
	if (rc != 0)
		goto exit;

	/* Convert the opal_errd process to a daemon. */
	if (opt_daemon) {
		rc = daemon(0, 0);
		if (rc) {
			syslog(LOG_NOTICE, "Cannot daemonize opal_errd, "
			       "opal_errd cannot continue.\n");
			goto exit;
		}

		siga.sa_handler = SIG_IGN;
		sigemptyset(&siga.sa_mask);
		siga.sa_flags = 0;
		sigaction(SIGHUP, &siga, NULL);

		/* Setup a signal handler for SIGTERM */
		siga.sa_handler = &term_handler;
		rc = sigaction(SIGTERM, &siga, NULL);
		if (rc) {
			syslog(LOG_NOTICE, "Could not initialize signal handler"
			       " for termination signal (SIGTERM), %s\n",
			       strerror(errno));
			goto exit;
		}
	}

	fds[INOTIFY_FD].events = POLLIN;
	fds[UDEV_FD].events = POLLIN;
	/* Read error/event log until we get termination signal */
	while (!terminate) {
		find_and_read_elog_events(elog_path, opt_output_dir);
		rotate_logs(opt_output_dir, opt_max_logs, opt_max_age);

		if (extract_opal_dump_cmd)
			check_platform_dump(extract_opal_dump_cmd,
					opt_sysfs, opt_max_dump);

		if (!opt_watch) {
			terminate = 1;
		} else {
			/* We don't care about the content of the inotify
			 * event, we'll just scan the directory anyway
			 */
			rc = poll(fds, sizeof(fds)/sizeof(struct pollfd), POLL_TIMEOUT);
			if (rc > 0 && fds[INOTIFY_FD].revents)
				read(fds[INOTIFY_FD].fd, inotifybuf, sizeof(inotifybuf));
			if (rc > 0 && fds[UDEV_FD].revents) {
				udev_dev = udev_monitor_receive_device(udev_mon);
				devpath = udev_device_get_devpath(udev_dev);
				if (devpath && strrchr(devpath, '/'))
					strncpy(elog_str_name, strrchr(devpath, '/'), ELOG_STR_SIZE);
				udev_device_unref(udev_dev);
				/* The id of the elog should be in elog_str_name
				 * Perhaps more can be done with the udev information
				 */
			}
		}
		rc = 0;
	}

exit:
	syslog(LOG_NOTICE, "Terminating\n");
	if (udev_mon)
		udev_monitor_unref(udev_mon);
	if (udev)
		udev_unref(udev);
	if (fds[INOTIFY_FD].fd >= 0)
		close(fds[INOTIFY_FD].fd);

	free(extract_opal_dump_cmd);
	closelog();
	return rc;
}
