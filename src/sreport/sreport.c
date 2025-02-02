/*****************************************************************************\
 *  sreport.c - report generating tool for slurm accounting.
 *****************************************************************************
 *  Portions Copyright (C) 2010-2017 SchedMD LLC.
 *  Copyright (C) 2008 Lawrence Livermore National Security.
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Danny Auble <da@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#include "src/sreport/sreport.h"
#include "src/sreport/cluster_reports.h"
#include "src/sreport/job_reports.h"
#include "src/sreport/resv_reports.h"
#include "src/sreport/user_reports.h"
#include "src/common/xsignal.h"
#include "src/common/proc_args.h"
#include "src/common/strlcpy.h"

#define OPT_LONG_LOCAL		0x101
#define OPT_LONG_FEDR		0x102
#define OPT_LONG_AUTOCOMP	0x103

char *command_name;
int exit_code;		/* sreport's exit code, =1 on any error at any time */
int exit_flag;		/* program to terminate if =1 */
char *fed_name = NULL;	/* Operating in federation mode */
bool federation_flag;	/* --federation option */
bool local_flag;	/* --local option */
int quiet_flag;		/* quiet=1, verbose=-1, normal=0 */
char *tres_str = NULL;	/* --tres= value */
List g_tres_list = NULL;/* TRES list from database -- unlatered */
List tres_list = NULL;  /* TRES list based of tres_str (--tres=str) */
int all_clusters_flag = 0;
char *cluster_flag = NULL;
slurmdb_report_time_format_t time_format = SLURMDB_REPORT_TIME_MINS;
char *time_format_string = "Minutes";
void *db_conn = NULL;
slurmdb_report_sort_t sort_flag = SLURMDB_REPORT_SORT_TIME;
char *tres_usage_str = "CPU";
/* by default, normalize all usernames to lower case */
bool user_case_norm = true;
bool node_tres = false;

static char *	_build_cluster_string(void);
static void	_build_tres_list(void);
static void	_cluster_rep (int argc, char **argv);
static int	_get_command (int *argc, char **argv);
static void	_job_rep (int argc, char **argv);
static void     _print_version( void );
static int	_process_command (int argc, char **argv);
static void	_resv_rep (int argc, char **argv);
static int      _set_sort(char *format);
static int      _set_time_format(char *format);
static void	_usage ( void );
static void	_user_rep (int argc, char **argv);

int
main (int argc, char **argv)
{
	int error_code = SLURM_SUCCESS, i, opt_char;
	log_options_t opts = LOG_OPTS_STDERR_ONLY ;
	int option_index;
	uint16_t persist_conn_flags = 0;
	static struct option long_options[] = {
		{"autocomplete", required_argument, 0, OPT_LONG_AUTOCOMP},
		{"all_clusters", 0, 0, 'a'},
		{"cluster",  1, 0, 'M'},
		{"federation", no_argument, 0, OPT_LONG_FEDR},
		{"help",     0, 0, 'h'},
		{"immediate",0, 0, 'i'},
		{"local",    no_argument, 0, OPT_LONG_LOCAL},
		{"noheader", 0, 0, 'n'},
		{"parsable", 0, 0, 'p'},
		{"parsable2",0, 0, 'P'},
		{"quiet",    0, 0, 'Q'},
		{"sort",     0, 0, 's'},
		{"tres",     1, 0, 'T'},
		{"usage",    0, 0, 'h'},
		{"verbose",  0, 0, 'v'},
		{"version",  0, 0, 'V'},
		{NULL,       0, 0, 0}
	};

	command_name      = argv[0];
	exit_code         = 0;
	exit_flag         = 0;
	federation_flag   = false;
	local_flag        = false;
	quiet_flag        = 0;
	slurm_init(NULL);
	log_init("sreport", opts, SYSLOG_FACILITY_DAEMON, NULL);

	/* Check to see if we are running a supported accounting plugin */
	if (!slurm_with_slurmdbd()) {
		fprintf(stderr,
		        "You are not running a supported accounting_storage plugin\n"
		        "Only 'accounting_storage/slurmdbd' is supported.\n");
		exit(1);
	}

	if (xstrstr(slurm_conf.fed_params, "fed_display"))
		federation_flag = true;

	if (getenv("SREPORT_CLUSTER")) {
		cluster_flag = xstrdup(optarg);
		local_flag = true;
	}
	if (getenv("SREPORT_FEDERATION"))
		federation_flag = true;
	if (getenv("SREPORT_LOCAL"))
		local_flag = true;
	tres_str = xstrdup(getenv("SREPORT_TRES"));

	while ((opt_char = getopt_long(argc, argv, "aM:hnpPQs:t:T:vV",
			long_options, &option_index)) != -1) {
		switch (opt_char) {
		case (int)'?':
			fprintf(stderr, "Try \"sreport --help\" "
				"for more information\n");
			exit(1);
			break;
		case (int)'h':
			_usage ();
			exit(exit_code);
			break;
		case (int)'a':
			all_clusters_flag = 1;
			break;
		case OPT_LONG_FEDR:
			federation_flag = true;
			break;
		case OPT_LONG_LOCAL:
			local_flag = true;
			break;
		case (int) 'M':
			cluster_flag = xstrdup(optarg);
			federation_flag = true;
			break;
		case (int)'n':
			print_fields_have_header = 0;
			break;
		case (int)'p':
			print_fields_parsable_print =
				PRINT_FIELDS_PARSABLE_ENDING;
			break;
		case (int)'P':
			print_fields_parsable_print =
				PRINT_FIELDS_PARSABLE_NO_ENDING;
			break;
		case (int)'Q':
			quiet_flag = 1;
			break;
		case (int)'s':
			_set_sort(optarg);
			break;
		case (int)'t':
			_set_time_format(optarg);
			break;
		case (int)'T':
			xfree(tres_str);
			tres_str = xstrdup(optarg);
			break;
		case (int)'v':
			quiet_flag = -1;
			break;
		case (int)'V':
			_print_version();
			exit(exit_code);
			break;
		case OPT_LONG_AUTOCOMP:
			suggest_completion(long_options, optarg);
			exit(0);
			break;
		default:
			fprintf(stderr, "getopt error, returned %c\n",
				opt_char);
			exit(1);
		}
	}

	i = 0;
	if (all_clusters_flag)
		i++;
	if (cluster_flag)
		i++;
	if (local_flag)
		i++;
	if (i > 1) {
		fprintf(stderr,
			"Only one cluster option can be used (--all_clusters OR --cluster OR --local)\n"),
		exit(1);
	}


	db_conn = slurmdb_connection_get(&persist_conn_flags);

	if (federation_flag && !all_clusters_flag && !cluster_flag &&
	    !local_flag)
		cluster_flag = _build_cluster_string();

	if (errno) {
		fatal("Problem connecting to the database: %m");
		exit(1);
	}

	if (persist_conn_flags & PERSIST_FLAG_P_USER_CASE)
		user_case_norm = false;

	_build_tres_list();

	/* We are only running a single command and exiting */
	if (optind < argc)
		error_code = _process_command(argc - optind, argv + optind);
	else {
		/* We are running interactively multiple commands */
		int input_field_count = 0;
		char **input_fields = xcalloc(MAX_INPUT_FIELDS, sizeof(char *));
		while (error_code == SLURM_SUCCESS) {
			error_code = _get_command(
				&input_field_count, input_fields);
			if (error_code || exit_flag)
				break;

			error_code = _process_command(
				input_field_count, input_fields);
			if (exit_flag)
				break;
		}
		xfree(input_fields);
	}

	if (exit_flag == 2)
		putchar('\n');

	/* Free the cluster grabbed from the -M option */
	xfree(cluster_flag);

	slurmdb_connection_close(&db_conn);
	acct_storage_g_fini();
	exit(exit_code);
}

static int _foreach_cluster_list_to_str(void *x, void *arg)
{
	slurmdb_cluster_rec_t *cluster = (slurmdb_cluster_rec_t *)x;
	char **out_str = (char **)arg;

	xassert(cluster);
	xassert(out_str);

	xstrfmtcat(*out_str, "%s%s", *out_str ? "," : "", cluster->name);

	return SLURM_SUCCESS;
}

static char *_build_cluster_string(void)
{
	char *cluster_str = NULL;
	slurmdb_federation_rec_t *fed = NULL;
	slurmdb_federation_cond_t fed_cond;
	List fed_list = NULL;
	List cluster_list = list_create(NULL);

	list_append(cluster_list, slurm_conf.cluster_name);
	slurmdb_init_federation_cond(&fed_cond, 0);
	fed_cond.cluster_list = cluster_list;

	if ((fed_list =
	     slurmdb_federations_get(db_conn, &fed_cond)) &&
	    list_count(fed_list) == 1) {
		fed = list_pop(fed_list);
		fed_name = xstrdup(fed->name);
		list_for_each(fed->cluster_list, _foreach_cluster_list_to_str,
			      &cluster_str);
	}
	slurm_destroy_federation_rec(fed);
	FREE_NULL_LIST(cluster_list);
	FREE_NULL_LIST(fed_list);

	return cluster_str;
}

static void _build_tres_list(void)
{
	list_itr_t *iter;
	slurmdb_tres_rec_t *tres;
	char *save_ptr = NULL, *tok;

	if (!g_tres_list) {
		slurmdb_tres_cond_t cond = {0};
		g_tres_list = slurmdb_tres_get(db_conn, &cond);
		if (!g_tres_list) {
			fatal("Problem getting TRES data: %m");
			exit(1);
		}
	}
	FREE_NULL_LIST(tres_list);

	tres_list = list_create(slurmdb_destroy_tres_rec);
	if (!tres_str) {
		int tres_cpu_id = TRES_CPU;
		slurmdb_tres_rec_t *tres2;
		if (!(tres = list_find_first(g_tres_list,
					     slurmdb_find_tres_in_list,
					     &tres_cpu_id)))
			fatal("Failed to find CPU TRES!");
		tres2 = slurmdb_copy_tres_rec(tres);
		list_append(tres_list, tres2);

		return;
	}

	tres_usage_str = "TRES";
	tok = strtok_r(tres_str, ",", &save_ptr);
	while (tok) {
		if (!xstrcasecmp(tok, "ALL")) {
			/* If ALL clean and add all to avoid duplicates */
			FREE_NULL_LIST(tres_list);
			tres_list = list_create(slurmdb_destroy_tres_rec);

			iter = list_iterator_create(g_tres_list);
			while ((tres = list_next(iter))) {
				slurmdb_tres_rec_t *tres2 =
					slurmdb_copy_tres_rec(tres);
				list_append(tres_list, tres2);
			}
			list_iterator_destroy(iter);

			break;
		}
		tres = list_find_first(g_tres_list,
				       slurmdb_find_tres_in_list_by_type,
				       tok);

		if (tres && !xstrcasecmp(tok, "node")) {
			if ((time_format == SLURMDB_REPORT_TIME_SECS_PER) ||
			    (time_format == SLURMDB_REPORT_TIME_MINS_PER) ||
			    (time_format == SLURMDB_REPORT_TIME_HOURS_PER) ||
			    (time_format == SLURMDB_REPORT_TIME_PERCENT))
				fatal("TRES node usage is no longer reported in percent format reports.  Please use TRES CPU instead.");
			else
				node_tres = true;
		}
		if (tres) {
			slurmdb_tres_rec_t *tres2 = slurmdb_copy_tres_rec(tres);
			list_append(tres_list, tres2);
		}

		tok = strtok_r(NULL, ",", &save_ptr);
	}

	if (!list_count(tres_list))
		fatal("No valid TRES given");
}

#if !HAVE_READLINE
/*
 * Alternative to readline if readline is not available
 */
static char *_getline(const char *prompt)
{
	char buf[4096];
	char *line;
	int len;

	printf("%s", prompt);

	/* Set "line" here to avoid a warning, discard later */
	line = fgets(buf, 4096, stdin);
	if (line == NULL)
		return NULL;
	len = strlen(buf);
	if ((len == 0) || (len >= 4096))
		return NULL;
	if (buf[len-1] == '\n')
		buf[len-1] = '\0';
	else
		len++;
	line = malloc(len);
	if (!line)
		return NULL;
	strlcpy(line, buf, len);
	return line;
}
#endif

/*
 * _job_rep - Reports having to do with jobs
 * IN argc - count of arguments
 * IN argv - list of arguments
 */
static void _job_rep (int argc, char **argv)
{
	int error_code = SLURM_SUCCESS;
	int command_len = strlen(argv[0]);

	/* For backwards compatibility we just look at the 1st char
	 * by default since Sizes was the original name */
	if (!xstrncasecmp(argv[0], "SizesByAccount", MAX(command_len, 1))) {
		error_code = job_sizes_grouped_by_acct(
			(argc - 1), &argv[1]);
	} else if (!xstrncasecmp(argv[0],
				 "SizesByWcKey", MAX(command_len, 8))) {
		error_code = job_sizes_grouped_by_wckey(
			(argc - 1), &argv[1]);
	} else if (!xstrncasecmp(argv[0],
				"SizesByAccountAndWcKey",
				MAX(command_len, 15))) {
		error_code = job_sizes_grouped_by_acct_and_wckey(
			(argc - 1), &argv[1]);
	} else {
		exit_code = 1;
		fprintf(stderr, "Not valid report %s\n", argv[0]);
		fprintf(stderr, "Valid job reports are, ");
		fprintf(stderr, "\"SizesByAccount, SizesByAccountAndWcKey, ");
		fprintf(stderr, "and  SizesByWckey\"\n");
	}

	if (error_code) {
		exit_code = 1;
	}
}

/*
 * _user_rep - Reports having to do with users
 * IN argc - count of arguments
 * IN argv - list of arguments
 */
static void _user_rep (int argc, char **argv)
{
	int error_code = SLURM_SUCCESS;

	if (xstrncasecmp(argv[0], "Top", 1) == 0) {
		error_code = user_top((argc - 1), &argv[1]);
	} else {
		exit_code = 1;
		fprintf(stderr, "Not valid report %s\n", argv[0]);
		fprintf(stderr, "Valid user reports are, ");
		fprintf(stderr, "\"Top\"\n");
	}

	if (error_code) {
		exit_code = 1;
	}
}

/*
 * _resv_rep - Reports having to do with reservations
 * IN argc - count of arguments
 * IN argv - list of arguments
 */
static void _resv_rep (int argc, char **argv)
{
	int error_code = SLURM_SUCCESS;

	if (xstrncasecmp(argv[0], "Utilization", 1) == 0) {
		error_code = resv_utilization((argc - 1), &argv[1]);
	} else {
		exit_code = 1;
		fprintf(stderr, "Not valid report %s\n", argv[0]);
		fprintf(stderr, "Valid reservation reports are, ");
		fprintf(stderr, "\"Utilization\"\n");
	}

	if (error_code) {
		exit_code = 1;
	}
}

/*
 * _cluster_rep - Reports having to do with clusters
 * IN argc - count of arguments
 * IN argv - list of arguments
 */
static void _cluster_rep (int argc, char **argv)
{
	int error_code = SLURM_SUCCESS;

	if (xstrncasecmp(argv[0], "AccountUtilizationByUser", 1) == 0) {
		error_code = cluster_account_by_user((argc - 1), &argv[1]);
	} else if ((xstrncasecmp(argv[0], "UserUtilizationByAccount", 18) == 0)
		   || (xstrncasecmp(argv[0], "UA", 2) == 0)) {
		error_code = cluster_user_by_account((argc - 1), &argv[1]);
	} else if ((xstrncasecmp(argv[0], "UserUtilizationByWckey", 18) == 0)
		   || (xstrncasecmp(argv[0], "UW", 2) == 0)) {
		error_code = cluster_user_by_wckey((argc - 1), &argv[1]);
	} else if (xstrncasecmp(argv[0], "Utilization", 2) == 0) {
		if (node_tres)
			fatal("TRES node usage is no longer reported in the Cluster Utilization report.  Please use TRES CPU instead.");
		error_code = cluster_utilization((argc - 1), &argv[1]);
	} else if (xstrncasecmp(argv[0], "WCKeyUtilizationByUser", 1) == 0) {
		error_code = cluster_wckey_by_user((argc - 1), &argv[1]);
	} else {
		exit_code = 1;
		fprintf(stderr, "Not valid report %s\n", argv[0]);
		fprintf(stderr, "Valid cluster reports are, ");
		fprintf(stderr, "\"AccountUtilizationByUser\", "
			"\"UserUtilizationByAccount\", "
			"\"UserUtilizationByWckey\", \"Utilization\", "
			"and \"WCKeyUtilizationByUser\"\n");
	}

	if (error_code) {
		exit_code = 1;
	}
}

/*
 * _get_command - get a command from the user
 * OUT argc - location to store count of arguments
 * OUT argv - location to store the argument list
 */
static int
_get_command (int *argc, char **argv)
{
	char *in_line;
	static char *last_in_line = NULL;
	int i, in_line_size;
	static int last_in_line_size = 0;

	*argc = 0;

#if HAVE_READLINE
	in_line = readline ("sreport: ");
#else
	in_line = _getline("sreport: ");
#endif
	if (in_line == NULL) {
		exit_flag = 2;
		return 0;
	}
	else if (xstrncmp (in_line, "#", 1) == 0) {
		free (in_line);
		return 0;
	} else if (xstrcmp (in_line, "!!") == 0) {
		free (in_line);
		in_line = last_in_line;
		in_line_size = last_in_line_size;
	} else {
		if (last_in_line)
			free (last_in_line);
		last_in_line = in_line;
		last_in_line_size = in_line_size = strlen (in_line);
	}

#if HAVE_READLINE
	add_history(in_line);
#endif

	/* break in_line into tokens */
	for (i = 0; i < in_line_size; i++) {
		bool double_quote = false, single_quote = false;
		if (in_line[i] == '\0')
			break;
		if (isspace ((int) in_line[i]))
			continue;
		if (((*argc) + 1) > MAX_INPUT_FIELDS) {	/* bogus input line */
			exit_code = 1;
			fprintf (stderr,
				 "%s: can not process over %d words\n",
				 command_name, MAX_INPUT_FIELDS - 1);
			return E2BIG;
		}
		argv[(*argc)++] = &in_line[i];
		for (i++; i < in_line_size; i++) {
			if (in_line[i] == '\042') {
				double_quote = !double_quote;
				continue;
			}
			if (in_line[i] == '\047') {
				single_quote = !single_quote;
				continue;
			}
			if (in_line[i] == '\0')
				break;
			if (double_quote || single_quote)
				continue;
			if (isspace ((int) in_line[i])) {
				in_line[i] = '\0';
				break;
			}
		}
	}
	return 0;
}


static void _print_version(void)
{
	print_slurm_version ();
	if (quiet_flag == -1) {
		long version = slurm_api_version();
		printf("slurm_api_version: %ld, %ld.%ld.%ld\n", version,
			SLURM_VERSION_MAJOR(version),
			SLURM_VERSION_MINOR(version),
			SLURM_VERSION_MICRO(version));
	}
}

/*
 * _process_command - process the user's command
 * IN argc - count of arguments
 * IN argv - the arguments
 * RET 0 or errno (only for errors fatal to sreport)
 */
static int
_process_command (int argc, char **argv)
{
	int command_len = 0;

	if (argc < 1) {
		exit_code = 1;
		if (quiet_flag == -1)
			fprintf(stderr, "no input");
		return 0;
	}

	command_len = strlen(argv[0]);

	if ((xstrncasecmp(argv[0], "cluster", MAX(command_len, 2)) == 0)) {
		if (argc < 2) {
			exit_code = 1;
			if (quiet_flag != 1)
				fprintf(stderr,
				        "too few arguments for keyword:%s\n",
				        argv[0]);
		} else
			_cluster_rep((argc - 1), &argv[1]);
	} else if (xstrncasecmp(argv[0], "help", MAX(command_len, 2)) == 0) {
		if (argc > 1) {
			exit_code = 1;
			fprintf (stderr,
				 "too many arguments for keyword:%s\n",
				 argv[0]);
		}
		_usage ();
	} else if ((xstrncasecmp(argv[0], "job", MAX(command_len, 1)) == 0)) {
		if (argc < 2) {
			exit_code = 1;
			if (quiet_flag != 1)
				fprintf(stderr,
				        "too few arguments for keyword:%s\n",
				        argv[0]);
		} else
			_job_rep((argc - 1), &argv[1]);
	} else if (xstrncasecmp(argv[0], "quiet", MAX(command_len, 4)) == 0) {
		if (argc > 1) {
			exit_code = 1;
			fprintf (stderr, "too many arguments for keyword:%s\n",
				 argv[0]);
		}
		quiet_flag = 1;
	} else if ((xstrncasecmp(argv[0], "exit", MAX(command_len, 1)) == 0) ||
		   (xstrncasecmp(argv[0], "\\q", MAX(command_len, 2)) == 0) ||
		   (xstrncasecmp(argv[0], "quit", MAX(command_len, 4)) == 0)) {
		if (argc > 1) {
			exit_code = 1;
			fprintf (stderr,
				 "too many arguments for keyword:%s\n",
				 argv[0]);
		}
		exit_flag = 1;
	} else if (xstrncasecmp(argv[0], "local", MAX(command_len, 3)) == 0) {
		if (argc > 1) {
			exit_code = 1;
			fprintf (stderr, "too many arguments for keyword:%s\n",
				 argv[0]);
		}
		local_flag = true;
	} else if (xstrncasecmp(argv[0], "nonparsable",
				MAX(command_len, 4)) == 0) {
		if (argc > 1) {
			exit_code = 1;
			fprintf (stderr, "too many arguments for keyword:%s\n",
				 argv[0]);
		}
		print_fields_parsable_print = 0;
	} else if (xstrncasecmp(argv[0], "parsable",
				MAX(command_len, 8)) == 0) {
		if (argc > 1) {
			exit_code = 1;
			fprintf (stderr, "too many arguments for keyword:%s\n",
				 argv[0]);
		}
		print_fields_parsable_print = PRINT_FIELDS_PARSABLE_ENDING;
	} else if (xstrncasecmp(argv[0], "parsable2",
				MAX(command_len, 9)) == 0) {
		if (argc > 1) {
			exit_code = 1;
			fprintf (stderr, "too many arguments for keyword:%s\n",
				 argv[0]);
		}
		print_fields_parsable_print = PRINT_FIELDS_PARSABLE_NO_ENDING;
	} else if ((xstrncasecmp(argv[0], "reservation",
				 MAX(command_len, 2)) == 0)
		   || (xstrncasecmp(argv[0], "resv",
				    MAX(command_len, 2)) == 0)) {
		if (argc < 2) {
			exit_code = 1;
			if (quiet_flag != 1)
				fprintf(stderr,
				        "too few arguments for keyword:%s\n",
				        argv[0]);
		} else
			_resv_rep((argc - 1), &argv[1]);
	} else if (xstrncasecmp(argv[0], "sort", MAX(command_len, 1)) == 0) {
		if (argc < 2) {
			exit_code = 1;
			fprintf (stderr,
				 "too few arguments for keyword:%s\n",
				 argv[0]);
		} else
			_set_sort(argv[1]);
	} else if (xstrncasecmp(argv[0], "time", MAX(command_len, 1)) == 0) {
		if (argc < 2) {
			exit_code = 1;
			fprintf (stderr,
				 "too few arguments for keyword:%s\n",
				 argv[0]);
		} else
			_set_time_format(argv[1]);
	} else if (xstrncasecmp(argv[0], "verbose", MAX(command_len, 4)) == 0) {
		if (argc > 1) {
			exit_code = 1;
			fprintf (stderr,
				 "too many arguments for %s keyword\n",
				 argv[0]);
		}
		quiet_flag = -1;
	} else if (xstrncasecmp(argv[0], "version", MAX(command_len, 4)) == 0) {
		if (argc > 1) {
			exit_code = 1;
			fprintf (stderr,
				 "too many arguments for %s keyword\n",
				 argv[0]);
		}
		_print_version();
	} else if ((xstrncasecmp(argv[0], "user", MAX(command_len, 1)) == 0)) {
		if (argc < 2) {
			exit_code = 1;
			if (quiet_flag != 1)
				fprintf(stderr,
				        "too few arguments for keyword:%s\n",
				        argv[0]);
		} else
			_user_rep((argc - 1), &argv[1]);
	} else {
		exit_code = 1;
		fprintf (stderr, "invalid keyword: %s\n", argv[0]);
	}

	return 0;
}

static int _set_time_format(char *format)
{
	int command_len = strlen(format);

	if (xstrncasecmp(format, "SecPer", MAX(command_len, 6)) == 0) {
		time_format = SLURMDB_REPORT_TIME_SECS_PER;
		time_format_string = "Seconds/Percentage of Total";
	} else if (xstrncasecmp(format, "MinPer", MAX(command_len, 6)) == 0) {
		time_format = SLURMDB_REPORT_TIME_MINS_PER;
		time_format_string = "Minutes/Percentage of Total";
	} else if (xstrncasecmp(format, "HourPer", MAX(command_len, 6)) == 0) {
		time_format = SLURMDB_REPORT_TIME_HOURS_PER;
		time_format_string = "Hours/Percentage of Total";
	} else if (xstrncasecmp(format, "Seconds", MAX(command_len, 1)) == 0) {
		time_format = SLURMDB_REPORT_TIME_SECS;
		time_format_string = "Seconds";
	} else if (xstrncasecmp(format, "Minutes", MAX(command_len, 1)) == 0) {
		time_format = SLURMDB_REPORT_TIME_MINS;
		time_format_string = "Minutes";
	} else if (xstrncasecmp(format, "Hours", MAX(command_len, 1)) == 0) {
		time_format = SLURMDB_REPORT_TIME_HOURS;
		time_format_string = "Hours";
	} else if (xstrncasecmp(format, "Percent", MAX(command_len, 1)) == 0) {
		time_format = SLURMDB_REPORT_TIME_PERCENT;
		time_format_string = "Percentage of Total";
	} else {
		fprintf (stderr, "unknown time format %s", format);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}

static int _set_sort(char *format)
{
	int command_len = strlen(format);

	if (xstrncasecmp(format, "Name", MAX(command_len, 1)) == 0) {
		sort_flag = SLURMDB_REPORT_SORT_NAME;
	} else if (xstrncasecmp(format, "Time", MAX(command_len, 6)) == 0) {
		sort_flag = SLURMDB_REPORT_SORT_TIME;
	} else {
		fprintf (stderr, "unknown timesort format %s", format);
		return SLURM_ERROR;
	}

	return SLURM_SUCCESS;
}


/* _usage - show the valid sreport commands */
void _usage (void) {
	printf ("\
sreport [<OPTION>] [<COMMAND>]                                             \n\
    Valid <OPTION> values are:                                             \n\
     -a or --all_clusters: Use all clusters instead of current             \n\
     --federation: Generate reports for the federation if a member of one  \n\
     -h or --help: equivalent to \"help\" command                          \n\
     --local: Report local cluster, even when in federation of clusters    \n\
     -n or --noheader: equivalent to \"noheader\" command                  \n\
     -p or --parsable: output will be '|' delimited with a '|' at the end  \n\
     -P or --parsable2: output will be '|' delimited without a '|' at the end\n\
     -Q or --quiet: equivalent to \"quiet\" command                        \n\
     -t <time_format>: Second, Minute, Hour, Percent, SecPer, MinPer, HourPer\n\
     -T or --tres: comma separated list of TRES, or 'ALL' for all TRES     \n\
     -v or --verbose: equivalent to \"verbose\" command                    \n\
     -V or --version: equivalent to \"version\" command                    \n\
                                                                           \n\
  <keyword> may be omitted from the execute line and sreport will execute  \n\
  in interactive mode. It will process commands as entered until explicitly\n\
  terminated.                                                              \n\
                                                                           \n\
    Valid <COMMAND> values are:                                            \n\
     exit                Terminate sreport                                 \n\
     help                Print this description of use.                    \n\
     nonparsable         Return output to normal                           \n\
     parsable            Output will be | delimited with an ending '|'     \n\
     parsable2           Output will be | delimited without an ending '|'  \n\
     quiet               Print no messages other than error messages.      \n\
     quit                Terminate this command.                           \n\
     time <time_format>  Second, Minute, Hour, Percent, SecPer, MinPer, HourPer\n\
     verbose             Enable detailed logging.                          \n\
     version             Display tool version number.                      \n\
     !!                  Repeat the last command entered.                  \n\
                                                                           \n\
    Valid report types are:                                                \n\
     cluster <REPORT> <OPTIONS>                                            \n\
     job <REPORT> <OPTIONS>                                                \n\
     user <REPORT> <OPTIONS>                                               \n\
                                                                           \n\
  <REPORT> is different for each report type.                              \n\
     cluster - AccountUtilizationByUser, UserUtilizationByAccount,         \n\
               UserUtilizationByWckey, Utilization, WCKeyUtilizationByUser \n\
     job     - SizesByAccount, SizesByAccountAndWckey, SizesByWckey        \n\
     reservation                                                           \n\
             - Utilization                                                 \n\
     user    - TopUsage                                                    \n\
                                                                           \n\
  <OPTIONS> are different for each report type.                            \n\
                                                                           \n\
     COMMON FOR ALL TYPES                                                  \n\
             - All_Clusters     - Use all monitored clusters default is    \n\
                                  local cluster.                           \n\
             - Clusters=<OPT>   - List of clusters to include in report    \n\
                                  Default is local cluster.                \n\
             - End=<OPT>        - Period ending for report.                \n\
                                  Default is 23:59:59 of previous day.     \n\
             - Format=<OPT>     - Comma separated list of fields to display\n\
                                  in report.                               \n\
             - Start=<OPT>      - Period start for report.                 \n\
                                  Default is 00:00:00 of previous day.     \n\
                                                                           \n\
     cluster - Accounts=<OPT>   - When used with the UserUtilizationByAccount,\n\
                                  or AccountUtilizationByUser, List of accounts\n\
                                  to include in report.  Default is all.   \n\
             - Tree             - When used with the AccountUtilizationByUser\n\
                                  report will span the accounts as they    \n\
                                  in the hierarchy.                        \n\
             - Users=<OPT>      - When used with any report other than     \n\
                                  Utilization, List of users to include in \n\
                                  report.  Default is all.                 \n\
             - Wckeys=<OPT>     - When used with the UserUtilizationByWckey\n\
                                  or WCKeyUtilizationByUser, List of wckeys\n\
                                  to include in report.  Default is all.   \n\
                                                                           \n\
     job     - Accounts=<OPT>   - List of accounts to use for the report.  \n\
                                  Default is all, which will show only     \n\
                                  one line corresponding to the totals of  \n\
                                  all accounts in the hierarchy.           \n\
                                  This explanation does not apply when ran \n\
                                  with the FlatView or AcctAsParent option.\n\
             - AcctAsParent     - When used with the SizesbyAccount(*)     \n\
                                  will take specified accounts as parents  \n\
                                  and the next layer of accounts under     \n\
                                  those specified will be displayed.       \n\
                                  Default is root if no Accounts specified.\n\
                                  When FlatView is used, this option is    \n\
                                  ignored.                                 \n\
             - FlatView         - When used with the SizesbyAccount(*)     \n\
                                  will not group accounts in a             \n\
                                  hierarchical level, but print each       \n\
                                  account where jobs ran on a separate     \n\
                                  line without any hierarchy.              \n\
             - GID=<OPT>        - List of group ids to include in report.  \n\
                                  Default is all.                          \n\
             - Grouping=<OPT>   - Comma separated list of size groupings.  \n\
                                  (i.e. 50,100,150 would group job cpu count\n\
                                   1-49, 50-99, 100-149, > 150).           \n\
                                  grouping=individual will result in a     \n\
                                  single column for each job size found.   \n\
             - Jobs=<OPT>       - List of jobs/steps to include in report. \n\
                                  Default is all.                          \n\
             - Nodes=<OPT>      - Only show jobs that ran on these nodes.  \n\
                                  Default is all.                          \n\
             - Partitions=<OPT> - List of partitions jobs ran on to include\n\
                                  in report.  Default is all.              \n\
             - PrintJobCount    - When used with the any Sizes report      \n\
                                  will print number of jobs ran instead of \n\
                                  time used.                               \n\
             - Users=<OPT>      - List of users jobs to include in report. \n\
                                  Default is all.                          \n\
             - Wckeys=<OPT>     - List of wckeys to use for the report.    \n\
                                  Default is all.  The SizesbyWckey        \n\
                                  report all users summed together.  If    \n\
                                  you want only certain users specify them \n\
                                  them with the Users= option.             \n\
                                                                           \n\
     reservation                                                           \n\
             - Names=<OPT>      - List of reservations to use for the report\n\
                                  Default is all.                          \n\
             - Nodes=<OPT>      - Only show reservations that used these   \n\
                                  nodes.  Default is all.                  \n\
                                                                           \n\
     user    - Accounts=<OPT>   - List of accounts to use for the report   \n\
                                  Default is all.                          \n\
             - Group            - Group all accounts together for each user.\n\
                                  Default is a separate entry for each user\n\
                                  and account reference.                   \n\
             - TopCount=<OPT>   - Used in the TopUsage report.  Change the \n\
                                  number of users displayed.  Default is 10.\n\
             - Users=<OPT>      - List of users jobs to include in report. \n\
                                  Default is all.                          \n\
                                                                           \n\
  Below are the format options for each report.                            \n\
                                                                           \n\
  One can get an number of characters by following the field option with   \n\
  a %%NUMBER option.  i.e. format=name%%30 will print 30 chars of field name.\n\
                                                                           \n\
       Cluster                                                             \n\
       - AccountUtilizationByUser                                          \n\
       - UserUtilizationByAccount                                          \n\
             - Accounts, Cluster, Count, Login, Proper, Used               \n\
       - UserUtilizationByWckey                                            \n\
       - WCKeyUtilizationByUser                                            \n\
             - Cluster, Count, Login, Proper, Used, Wckey                  \n\
       - Utilization                                                       \n\
             - Allocated, Cluster, Count, Down, Idle, Overcommitted,       \n\
               Planned, PlannedDown, Reported                              \n\
                                                                           \n\
       Job                                                                 \n\
       - Sizes                                                             \n\
             - Account, Cluster                                            \n\
                                                                           \n\
       Reservation                                                         \n\
       - Utilization                                                       \n\
             - Allocated, Associations, Cluster, Count, CPUTime,           \n\
               End, Flags, Idle, Name, Nodes, ReservationId, Start, TotalTime \n\
                                                                           \n\
       User                                                                \n\
       - TopUsage                                                          \n\
             - Account, Cluster, Login, Proper, Used                       \n\
                                                                           \n\
                                                                           \n\
  Note, valid start/end time formats are...                                \n\
       HH:MM[:SS] [AM|PM]                                                  \n\
       MMDD[YY] or MM/DD[/YY] or MM.DD[.YY]                                \n\
       MM/DD[/YY]-HH:MM[:SS]                                               \n\
       YYYY-MM-DD[THH:MM[:SS]]                                             \n\
       now[{+|-}count[seconds(default)|minutes|hours|days|weeks]]          \n\
                                                                           \n\
                                                                           \n\
  All commands and options are case-insensitive.                         \n\n");

}
