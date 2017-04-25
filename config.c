/*
 * config.c - parse repmgr.conf and other configuration-related functionality
 *
 * Copyright (c) 2ndQuadrant, 2010-2017
 */

#include <sys/stat.h>			/* for stat() */

#include "repmgr.h"
#include "config.h"
#include "log.h"

const static char *_progname = NULL;
static char config_file_path[MAXPGPATH];
static bool config_file_provided = false;
bool config_file_found = false;

static void	_parse_config(t_configuration_options *options, ItemList *error_list, ItemList *warning_list);
static void	_parse_line(char *buf, char *name, char *value);
static void	parse_event_notifications_list(t_configuration_options *options, const char *arg);
static void	tablespace_list_append(t_configuration_options *options, const char *arg);

static char	*trim(char *s);

static void	exit_with_errors(ItemList *config_errors, ItemList *config_warnings);


void
set_progname(const char *argv0)
{
	_progname = get_progname(argv0);
}

const char *
progname(void)
{
	return _progname;
}

bool
load_config(const char *config_file, bool verbose, t_configuration_options *options, char *argv0)
{
	struct stat stat_config;

	/*
	 * If a configuration file was provided, check it exists, otherwise
	 * emit an error and terminate. We assume that if a user explicitly
	 * provides a configuration file, they'll want to make sure it's
	 * used and not fall back to any of the defaults.
	 */
	if (config_file[0])
	{
		strncpy(config_file_path, config_file, MAXPGPATH);
		canonicalize_path(config_file_path);

		if (stat(config_file_path, &stat_config) != 0)
		{
			log_error(_("provided configuration file \"%s\" not found: %s"),
					config_file,
					strerror(errno)
				);
			exit(ERR_BAD_CONFIG);
		}

		if (verbose == true)
		{
			log_notice(_("using configuration file \"%s\""), config_file);
		}

		config_file_provided = true;
		config_file_found = true;
	}


	/*
	 * If no configuration file was provided, attempt to find a default file
	 * in this order:
	 *	- current directory
	 *	- /etc/repmgr.conf
	 *	- default sysconfdir
	 *
	 * here we just check for the existence of the file; parse_config()
	 * will handle read errors etc.
	 *
	 * XXX modify this section so package maintainers can provide a patch
	 *	   specifying location of a distribution-specific configuration file
	 */
	if (config_file_provided == false)
	{
		char		my_exec_path[MAXPGPATH];
		char		sysconf_etc_path[MAXPGPATH];

		/* 1. "./repmgr.conf" */
		if (verbose == true)
		{
			log_notice(_("looking for configuration file in current directory"));
		}

		snprintf(config_file_path, MAXPGPATH, "./%s", CONFIG_FILE_NAME);
		canonicalize_path(config_file_path);

		if (stat(config_file_path, &stat_config) == 0)
		{
			config_file_found = true;
			goto end_search;
		}

		/* 2. "/etc/repmgr.conf" */
		if (verbose == true)
		{
			log_notice(_("looking for configuration file in /etc"));
		}

		snprintf(config_file_path, MAXPGPATH, "/etc/%s", CONFIG_FILE_NAME);
		if (stat(config_file_path, &stat_config) == 0)
		{
			config_file_found = true;
			goto end_search;
		}

		/* 3. default sysconfdir */
		if (find_my_exec(argv0, my_exec_path) < 0)
		{
			log_error(_("%s: could not find own program executable"), argv0);
			exit(EXIT_FAILURE);
		}

		get_etc_path(my_exec_path, sysconf_etc_path);

		if (verbose == true)
		{
			log_notice(_("looking for configuration file in %s"), sysconf_etc_path);
		}

		snprintf(config_file_path, MAXPGPATH, "%s/%s", sysconf_etc_path, CONFIG_FILE_NAME);
		if (stat(config_file_path, &stat_config) == 0)
		{
			config_file_found = true;
			goto end_search;
		}

	end_search:
		if (config_file_found == true)
		{
			if (verbose == true)
			{
				log_notice(_("configuration file found at: %s"), config_file_path);
			}
		}
		else
		{
			if (verbose == true)
			{
				log_notice(_("no configuration file provided or found"));
			}
		}
	}

	return parse_config(options);
}


bool
parse_config(t_configuration_options *options)
{
	/* Collate configuration file errors here for friendlier reporting */
	static ItemList config_errors = { NULL, NULL };
	static ItemList config_warnings = { NULL, NULL };

	_parse_config(options, &config_errors, &config_warnings);

	/* errors found - exit after printing details, and any warnings */
	if (config_errors.head != NULL)
	{
		exit_with_errors(&config_errors, &config_warnings);
	}

	if (config_warnings.head != NULL)
	{
		ItemListCell *cell;

		log_warning(_("the following problems were found in the configuration file:"));
		for (cell = config_warnings.head; cell; cell = cell->next)
		{
			fprintf(stderr, "  %s\n", cell->string);
		}
	}

	return true;
}


static void
_parse_config(t_configuration_options *options, ItemList *error_list, ItemList *warning_list)
{
	FILE	   *fp;
	char	   *s,
				buf[MAXLINELENGTH];
	char		name[MAXLEN];
	char		value[MAXLEN];

	bool		node_id_found = false;

	/* Initialize configuration options with sensible defaults */

	/* node information */
	options->node_id = UNKNOWN_NODE_ID;
	options->upstream_node_id = NO_UPSTREAM_NODE;
	memset(options->node_name, 0, sizeof(options->node_name));
	memset(options->conninfo, 0, sizeof(options->conninfo));
	memset(options->pg_bindir, 0, sizeof(options->pg_bindir));
	options->replication_type = REPLICATION_TYPE_PHYSICAL;

	/*
	 * log settings
	 *
	 * note: the default for "loglevel" is set in log.c and does not need
	 * to be initialised here
	 */
	memset(options->logfacility, 0, sizeof(options->logfacility));
	memset(options->logfile, 0, sizeof(options->logfile));

	/* standby clone settings
	 * ----------------------- */
	options->use_replication_slots = false;
	memset(options->rsync_options, 0, sizeof(options->rsync_options));
	memset(options->ssh_options, 0, sizeof(options->ssh_options));
	memset(options->pg_basebackup_options, 0, sizeof(options->pg_basebackup_options));
	memset(options->restore_command, 0, sizeof(options->restore_command));
	options->tablespace_mapping.head = NULL;
	options->tablespace_mapping.tail = NULL;

	/* repmgrd settings
	 * ---------------- */
	options->failover_mode = MANUAL_FAILOVER;
	options->priority = DEFAULT_PRIORITY;
	memset(options->promote_command, 0, sizeof(options->promote_command));
	memset(options->follow_command, 0, sizeof(options->follow_command));
	options->monitor_interval_secs = 2;
	options->master_response_timeout = 60;
	/* default to 6 reconnection attempts at intervals of 10 seconds */
	options->reconnect_attempts = 6;
	options->reconnect_interval = 10;
	options->retry_promote_interval_secs = 300;
	options->monitoring_history = false;  /* new in 4.0, replaces --monitoring-history */

	/* witness settings
	 * ---------------- */

	/* default to resyncing repl_nodes table every 30 seconds on the witness server */
	options->witness_repl_nodes_sync_interval_secs = 30;

	/* service settings
	 * ---------------- */
	memset(options->pg_ctl_options, 0, sizeof(options->pg_ctl_options));
	memset(options->service_stop_command, 0, sizeof(options->service_stop_command));
	memset(options->service_start_command, 0, sizeof(options->service_start_command));
	memset(options->service_restart_command, 0, sizeof(options->service_restart_command));
	memset(options->service_reload_command, 0, sizeof(options->service_reload_command));
	memset(options->service_promote_command, 0, sizeof(options->service_promote_command));

	/* event notification settings
	 * --------------------------- */
	memset(options->event_notification_command, 0, sizeof(options->event_notification_command));
	options->event_notifications.head = NULL;
	options->event_notifications.tail = NULL;

	/* barman settings */
	memset(options->barman_server, 0, sizeof(options->barman_server));
	memset(options->barman_config, 0, sizeof(options->barman_config));

	/*
	 * If no configuration file available (user didn't specify and none found
	 * in the default locations), return with default values
	 */
	if (config_file_found == false)
	{
		log_verbose(LOG_NOTICE,
					_("no configuration file provided and no default file found - "
					 "continuing with default values"));
		return;
	}

	fp = fopen(config_file_path, "r");

	/*
	 * A configuration file has been found, either provided by the user
	 * or found in one of the default locations. If we can't open it,
	 * fail with an error.
	 */
	if (fp == NULL)
	{
		if (config_file_provided)
		{
			log_error(_("unable to open provided configuration file \"%s\"; terminating"),
					  config_file_path);
		}
		else
		{
			log_error(_("unable to open default configuration file	\"%s\"; terminating"),
					config_file_path);
		}

		exit(ERR_BAD_CONFIG);
	}

	/* Read file */
	while ((s = fgets(buf, sizeof buf, fp)) != NULL)
	{
		bool known_parameter = true;

		/* Parse name/value pair from line */
		_parse_line(buf, name, value);

		/* Skip blank lines */
		if (!strlen(name))
			continue;

		/* Skip comments */
		if (name[0] == '#')
			continue;

		/* Copy into correct entry in parameters struct */
		if (strcmp(name, "node_id") == 0)
		{
			options->node_id = repmgr_atoi(value, name, error_list, 1);
			node_id_found = true;
		}
		else if (strcmp(name, "node_name") == 0)
			strncpy(options->node_name, value, MAXLEN);
		else if (strcmp(name, "upstream_node_id") == 0)
			options->upstream_node_id = repmgr_atoi(value, name, error_list, 1);
		else if (strcmp(name, "conninfo") == 0)
			strncpy(options->conninfo, value, MAXLEN);
		else if (strcmp(name, "pg_bindir") == 0)
			strncpy(options->pg_bindir, value, MAXLEN);
		else if (strcmp(name, "replication_type") == 0)
		{
			if (strcmp(value, "physical") == 0)
				options->replication_type = REPLICATION_TYPE_PHYSICAL;
			else if (strcmp(value, "bdr") == 0)
				options->replication_type = REPLICATION_TYPE_BDR;
			else
				item_list_append(error_list, _("value for 'replication_type' must be 'physical' or 'bdr'"));
		}

		/* log settings */
		else if (strcmp(name, "logfile") == 0)
			strncpy(options->logfile, value, MAXLEN);
		else if (strcmp(name, "loglevel") == 0)
			strncpy(options->loglevel, value, MAXLEN);
		else if (strcmp(name, "logfacility") == 0)
			strncpy(options->logfacility, value, MAXLEN);

		/* standby clone settings */
		else if (strcmp(name, "use_replication_slots") == 0)
			options->use_replication_slots = parse_bool(value, name, error_list);
		else if (strcmp(name, "rsync_options") == 0)
			strncpy(options->rsync_options, value, MAXLEN);
		else if (strcmp(name, "ssh_options") == 0)
			strncpy(options->ssh_options, value, MAXLEN);
		else if (strcmp(name, "pg_basebackup_options") == 0)
			strncpy(options->pg_basebackup_options, value, MAXLEN);
		else if (strcmp(name, "tablespace_mapping") == 0)
			tablespace_list_append(options, value);
		else if (strcmp(name, "restore_command") == 0)
			strncpy(options->restore_command, value, MAXLEN);

		/* repmgrd settings */
		else if (strcmp(name, "failover_mode") == 0)
		{
			if (strcmp(value, "manual") == 0)
			{
				options->failover_mode = MANUAL_FAILOVER;
			}
			else if (strcmp(value, "automatic") == 0)
			{
				options->failover_mode = AUTOMATIC_FAILOVER;
			}
			else
			{
				item_list_append(error_list, _("value for 'failover' must be 'automatic' or 'manual'\n"));
			}
		}
		else if (strcmp(name, "priority") == 0)
			options->priority = repmgr_atoi(value, name, error_list, 0);
		else if (strcmp(name, "promote_command") == 0)
			strncpy(options->promote_command, value, MAXLEN);
		else if (strcmp(name, "follow_command") == 0)
			strncpy(options->follow_command, value, MAXLEN);
		else if (strcmp(name, "reconnect_attempts") == 0)
			options->reconnect_attempts = repmgr_atoi(value, name, error_list, 0);
		else if (strcmp(name, "reconnect_interval") == 0)
			options->reconnect_interval = repmgr_atoi(value, name, error_list, 0);
		else if (strcmp(name, "monitor_interval_secs") == 0)
			options->monitor_interval_secs = repmgr_atoi(value, name, error_list, 1);
		else if (strcmp(name, "retry_promote_interval_secs") == 0)
			options->retry_promote_interval_secs = repmgr_atoi(value, name, error_list, 1);
		else if (strcmp(name, "monitoring_history") == 0)
			options->monitoring_history = parse_bool(value, name, error_list);

		/* witness settings */
		else if (strcmp(name, "witness_repl_nodes_sync_interval_secs") == 0)
			options->witness_repl_nodes_sync_interval_secs = repmgr_atoi(value, name, error_list, 1);

		/* service settings */
		else if (strcmp(name, "pg_ctl_options") == 0)
			strncpy(options->pg_ctl_options, value, MAXLEN);
		else if (strcmp(name, "service_stop_command") == 0)
			strncpy(options->service_stop_command, value, MAXLEN);
		else if (strcmp(name, "service_start_command") == 0)
			strncpy(options->service_start_command, value, MAXLEN);
		else if (strcmp(name, "service_restart_command") == 0)
			strncpy(options->service_restart_command, value, MAXLEN);
		else if (strcmp(name, "service_reload_command") == 0)
			strncpy(options->service_reload_command, value, MAXLEN);
		else if (strcmp(name, "service_promote_command") == 0)
			strncpy(options->service_promote_command, value, MAXLEN);

		/* event notification settings */
		else if (strcmp(name, "event_notification_command") == 0)
			strncpy(options->event_notification_command, value, MAXLEN);
		else if (strcmp(name, "event_notifications") == 0)
			parse_event_notifications_list(options, value);

		/* bdr settings */
		else if (strcmp(name, "bdr_monitoring_mode") == 0)
		{
			if (strncmp(value, "local", MAXLEN) == 0)
			{
				options->bdr_monitoring_mode = BDR_MONITORING_LOCAL;
			}
			else if (strcmp(value, "highest_priority") == 0)
			{
				options->bdr_monitoring_mode = BDR_MONITORING_PRIORITY;
			}
			else
			{
				item_list_append(error_list, _("value for 'bdr_monitoring_mode' must be 'local' or 'highest_priority'\n"));
			}
		}

		/* barman settings */
		else if (strcmp(name, "barman_server") == 0)
			strncpy(options->barman_server, value, MAXLEN);
		else if (strcmp(name, "barman_config") == 0)
			strncpy(options->barman_config, value, MAXLEN);

		/* Following parameters have been deprecated or renamed from 3.x - issue a warning */
		else if (strcmp(name, "cluster") == 0)
		{
			item_list_append(warning_list,
							 _("parameter \"cluster\" is deprecated and will be ignored"));
			known_parameter = false;
		}
		else if (strcmp(name, "failover") == 0)
		{
			item_list_append(warning_list,
							 _("parameter \"failover\" has been renamed 'failover_mode'"));
			known_parameter = false;
		}
		else if (strcmp(name, "node") == 0)
		{
			item_list_append(warning_list,
							 _("parameter \"node\" has been renamed 'node_id'"));
			known_parameter = false;
		}
		else if (strcmp(name, "upstream_node") == 0)
		{
			item_list_append(warning_list,
							 _("parameter \"upstream_node\" has been renamed 'upstream_node_id'"));
			known_parameter = false;
		}
		else
		{
			known_parameter = false;
			log_warning(_("%s/%s: unknown name/value pair provided; ignoring\n"), name, value);
		}
		/*
		 * Raise an error if a known parameter is provided with an empty value.
		 * Currently there's no reason why empty parameters are needed; if
		 * we want to accept those, we'd need to add stricter default checking,
		 * as currently e.g. an empty `node` value will be converted to '0'.
		 */
		if (known_parameter == true && !strlen(value)) {
			char	   error_message_buf[MAXLEN] = "";
			snprintf(error_message_buf,
					 MAXLEN,
					 _("\"%s\": no value provided"),
					 name);

			item_list_append(error_list, error_message_buf);
		}
	}


	/* check required parameters */
	if (node_id_found == false)
	{
		item_list_append(error_list, _("\"node_id\": required parameter was not found"));
	}

	if (!strlen(options->node_name))
	{
		item_list_append(error_list, _("\"node_name\": required parameter was not found"));
	}

	if (!strlen(options->conninfo))
	{
		item_list_append(error_list, _("\"conninfo\": required parameter was not found"));
	}
	else
	{
		/* Sanity check the provided conninfo string
		 *
		 * NOTE: PQconninfoParse() verifies the string format and checks for valid options
		 * but does not sanity check values
		 */

		PQconninfoOption *conninfo_options;
		char	   *conninfo_errmsg = NULL;

		conninfo_options = PQconninfoParse(options->conninfo, &conninfo_errmsg);
		if (conninfo_options == NULL)
		{
			char	   error_message_buf[MAXLEN] = "";
			snprintf(error_message_buf,
					 MAXLEN,
					 _("\"conninfo\": %s	(provided: \"%s\")"),
					 conninfo_errmsg,
					 options->conninfo);

			item_list_append(error_list, error_message_buf);
		}

		PQconninfoFree(conninfo_options);
	}
}


void
_parse_line(char *buf, char *name, char *value)
{
	int			i = 0;
	int			j = 0;

	/*
	 * Extract parameter name, if present
	 */
	for (; i < MAXLEN; ++i)
	{
		if (buf[i] == '=')
			break;

		switch(buf[i])
		{
			/* Ignore whitespace */
			case ' ':
			case '\n':
			case '\r':
			case '\t':
				continue;
			default:
				name[j++] = buf[i];
		}
	}
	name[j] = '\0';

	/*
	 * Ignore any whitespace following the '=' sign
	 */
	for (; i < MAXLEN; ++i)
	{
		if (buf[i+1] == ' ')
			continue;
		if (buf[i+1] == '\t')
			continue;

		break;
	}

	/*
	 * Extract parameter value
	 */
	j = 0;
	for (++i; i < MAXLEN; ++i)
		if (buf[i] == '\'')
			continue;
		else if (buf[i] == '#')
			break;
		else if (buf[i] != '\n')
			value[j++] = buf[i];
		else
			break;
	value[j] = '\0';
	trim(value);
}

static char *
trim(char *s)
{
	/* Initialize start, end pointers */
	char	   *s1 = s,
			   *s2 = &s[strlen(s) - 1];

	/* If string is empty, no action needed */
	if (s2 < s1)
		return s;

	/* Trim and delimit right side */
	while ((isspace(*s2)) && (s2 >= s1))
		--s2;
	*(s2 + 1) = '\0';

	/* Trim left side */
	while ((isspace(*s1)) && (s1 < s2))
		++s1;

	/* Copy finished string */
	memmove(s, s1, s2 - s1);
	s[s2 - s1 + 1] = '\0';

	return s;
}


bool
reload_config(t_configuration_options *orig_options)
{
	return true;
}


/* TODO: don't emit warnings if --terse and no errors */
static void
exit_with_errors(ItemList *config_errors, ItemList *config_warnings)
{
	ItemListCell *cell;

	log_error(_("following errors were found in the configuration file:"));

	for (cell = config_errors->head; cell; cell = cell->next)
	{
		fprintf(stderr, "  %s\n", cell->string);
	}

	if (config_warnings->head != NULL)
	{
		puts("");
		log_warning(_("the following problems were also found in the configuration file:"));
		for (cell = config_warnings->head; cell; cell = cell->next)
		{
			fprintf(stderr, "  %s\n", cell->string);
		}
	}
	exit(ERR_BAD_CONFIG);
}




/*
 * Convert provided string to an integer using strtol;
 * on error, if a callback is provided, pass the error message to that,
 * otherwise exit
 */
int
repmgr_atoi(const char *value, const char *config_item, ItemList *error_list, int minval)
{
	char	  *endptr;
	long	   longval = 0;
	PQExpBufferData errors;

	initPQExpBuffer(&errors);

	/* It's possible that some versions of strtol() don't treat an empty
	 * string as an error.
	 */
	if (*value == '\0')
	{
		/* don't log here - empty values will be caught later */
		return 0;
	}
	else
	{
		errno = 0;
		longval = strtol(value, &endptr, 10);

		if (value == endptr || errno)
		{
			appendPQExpBuffer(&errors,
							  _("\"%s\": invalid value (provided: \"%s\")"),
							  config_item, value);
		}
		else if ((int32)longval < longval)
		{
			appendPQExpBuffer(&errors,
							  _("\"%s\": must be a positive signed 32 bit integer, i.e. 2147483647 or less (provided: \"%s\")"),
							  config_item,
							  value);
		}
		else if ((int32)longval < minval)
			/* Disallow negative values for most parameters */
		{
			appendPQExpBuffer(&errors,
							  _("\"%s\": must be %i or greater (provided: \"%s\")"),
							  config_item,
							  minval,
							  value);
		}
	}

	/* Error message buffer is set */
	if (errors.data[0] != '\0')
	{
		if (error_list == NULL)
		{
			log_error("%s", errors.data);
			termPQExpBuffer(&errors);
			exit(ERR_BAD_CONFIG);
		}

		item_list_append(error_list, errors.data);
		termPQExpBuffer(&errors);
	}

	return (int32) longval;
}


/*
 * Interpret a parameter value as a boolean. Currently accepts:
 *
 * - true/false
 * - 0/1
 * - on/off
 *
 * Returns 'false' if unable to determine the booleanness of the value
 * and adds an entry to the error list, which will result in the program
 * erroring out before it proceeds to do anything.
 *
 * TODO: make this work like in postgresql.conf
 */
bool
parse_bool(const char *s, const char *config_item, ItemList *error_list)
{
	PQExpBufferData errors;

	if (strcmp(s, "0") == 0)
		return false;

	if (strcmp(s, "1") == 0)
		return true;

	if (strcmp(s, "false") == 0)
		return false;

	if (strcmp(s, "true") == 0)
		return true;

	if (strcmp(s, "off") == 0)
		return false;

	if (strcmp(s, "on") == 0)
		return true;

	initPQExpBuffer(&errors);

	appendPQExpBuffer(&errors,
					  "\"%s\": unable to interpret '%s' as a boolean value",
					  config_item, s);
	item_list_append(error_list, errors.data);
	termPQExpBuffer(&errors);

	return false;
}

/*
 * Split argument into old_dir and new_dir and append to tablespace mapping
 * list.
 *
 * Adapted from pg_basebackup.c
 */
static void
tablespace_list_append(t_configuration_options *options, const char *arg)
{
	TablespaceListCell *cell;
	char	   *dst;
	char	   *dst_ptr;
	const char *arg_ptr;

	cell = (TablespaceListCell *) pg_malloc0(sizeof(TablespaceListCell));
	if (cell == NULL)
	{
		log_error(_("unable to allocate memory; terminating"));
		exit(ERR_BAD_CONFIG);
	}

	dst_ptr = dst = cell->old_dir;
	for (arg_ptr = arg; *arg_ptr; arg_ptr++)
	{
		if (dst_ptr - dst >= MAXPGPATH)
		{
			log_error(_("directory name too long"));
			exit(ERR_BAD_CONFIG);
		}

		if (*arg_ptr == '\\' && *(arg_ptr + 1) == '=')
			;					/* skip backslash escaping = */
		else if (*arg_ptr == '=' && (arg_ptr == arg || *(arg_ptr - 1) != '\\'))
		{
			if (*cell->new_dir)
			{
				log_error(_("multiple \"=\" signs in tablespace mapping"));
				exit(ERR_BAD_CONFIG);
			}
			else
			{
				dst = dst_ptr = cell->new_dir;
			}
		}
		else
			*dst_ptr++ = *arg_ptr;
	}

	if (!*cell->old_dir || !*cell->new_dir)
	{
		log_error(_("invalid tablespace mapping format \"%s\", must be \"OLDDIR=NEWDIR\""),
				arg);
		exit(ERR_BAD_CONFIG);
	}

	canonicalize_path(cell->old_dir);
	canonicalize_path(cell->new_dir);

	if (options->tablespace_mapping.tail)
		options->tablespace_mapping.tail->next = cell;
	else
		options->tablespace_mapping.head = cell;

	options->tablespace_mapping.tail = cell;
}



/*
 * parse_event_notifications_list()
 *
 *
 */

static void
parse_event_notifications_list(t_configuration_options *options, const char *arg)
{
	const char *arg_ptr;
	char		event_type_buf[MAXLEN] = "";
	char	   *dst_ptr = event_type_buf;


	for (arg_ptr = arg; arg_ptr <= (arg + strlen(arg)); arg_ptr++)
	{
		/* ignore whitespace */
		if (*arg_ptr == ' ' || *arg_ptr == '\t')
		{
			continue;
		}

		/*
		 * comma (or end-of-string) should mark the end of an event type -
		 * just as long as there was something preceding it
		 */
		if ((*arg_ptr == ',' || *arg_ptr == '\0') && event_type_buf[0] != '\0')
		{
			EventNotificationListCell *cell;

			cell = (EventNotificationListCell *) pg_malloc0(sizeof(EventNotificationListCell));

			if (cell == NULL)
			{
				log_error(_("unable to allocate memory; terminating"));
				exit(ERR_BAD_CONFIG);
			}

			strncpy(cell->event_type, event_type_buf, MAXLEN);

			if (options->event_notifications.tail)
			{
				options->event_notifications.tail->next = cell;
			}
			else
			{
				options->event_notifications.head = cell;
			}

			options->event_notifications.tail = cell;

			memset(event_type_buf, 0, MAXLEN);
			dst_ptr = event_type_buf;
		}
		/* ignore duplicated commas */
		else if (*arg_ptr == ',')
		{
			continue;
		}
		else
		{
			*dst_ptr++ = *arg_ptr;
		}
	}
}
