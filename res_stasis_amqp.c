/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright 2013-2024 The Wazo Authors  (see the AUTHORS file)
 *
 * David M. Lee, II <dlee@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*!
 * \brief Statsd channel stats. Exmaple of how to subscribe to Stasis events.
 *
 * This module subscribes to the channel caching topic and issues statsd stats
 * based on the received messages.
 *
 * \author David M. Lee, II <dlee@digium.com>
 * \since 12
 */

/*** MODULEINFO
	<depend>res_stasis_amqp</depend>
	<defaultenabled>no</defaultenabled>
	<support_level>extended</support_level>
 ***/

/*** DOCUMENTATION
	<configInfo name="res_stasis_amqp" language="en_US">
		<synopsis>Stasis to AMQP Backend</synopsis>
		<configFile name="stasis_amqp.conf">
			<configObject name="global">
				<synopsis>Global configuration settings</synopsis>
				<configOption name="loguniqueid">
					<synopsis>Determines whether to log the uniqueid for calls</synopsis>
					<description>
						<para>Default is no.</para>
					</description>
				</configOption>
				<configOption name="connection">
					<synopsis>Name of the connection from amqp.conf to use</synopsis>
					<description>
						<para>Specifies the name of the connection from amqp.conf to use</para>
					</description>
				</configOption>
				<configOption name="exchange">
					<synopsis>Name of the exchange to post to</synopsis>
					<description>
						<para>Defaults to empty string</para>
					</description>
				</configOption>
				<configOption name="publish_ami_events">
					<synopsis>Whether or not ami events should be published</synopsis>
					<description>
						<para>Defaults to "yes"</para>
					</description>
				</configOption>
				<configOption name="publish_channel_events">
					<synopsis>Whether or not channel events should be published</synopsis>
					<description>
						<para>Defaults to "yes"</para>
					</description>
				</configOption>
				<configOption name="exclude_events">
					<synopsis>Exclude an optional comma-separated list of event name from any source (ami, stasis app, stasis channel)</synopsis>
					<description>
						<para>Defaults to empty list</para>
					</description>
				</configOption>
				<configOption name="include_channelvarset_events">
					<synopsis>Include an optional comma-separated list of ChannelVarset variable. If empty, all ChannelVarset events are included</synopsis>
					<description>
						<para>Defaults to empty list</para>
					</description>
				</configOption>
			</configObject>
		</configFile>
	</configInfo>
 ***/


#include "asterisk.h"

#include "asterisk/module.h"
#include "asterisk/stasis.h"
#include "asterisk/stasis_amqp.h"
#include "asterisk/stasis_channels.h"
#include "asterisk/stasis_app.h"
#include "asterisk/stasis_message_router.h"
#include "asterisk/stasis_bridges.h"
#include "asterisk/ari.h"
#include "asterisk/time.h"
#include "asterisk/config_options.h"
#include "asterisk/manager.h"
#include "asterisk/json.h"
#include "asterisk/utils.h"
#include "asterisk/hashtab.h"

#include "asterisk/amqp.h"

#define CONF_FILENAME "stasis_amqp.conf"
#define ROUTING_KEY_LEN 256

/*!
 * The ast_sched_context used for stasis application polling
 */
static struct ast_sched_context *stasis_app_sched_context;
struct ao2_container *registered_apps = NULL;

/*! Regular Stasis subscription */
static struct stasis_subscription *sub;
static struct stasis_subscription *manager;

static int setup_amqp(void);
static int publish_to_amqp(struct ast_json *body, struct ast_json *headers, const char *routing_key);
int register_to_new_stasis_app(const void *data);
char *new_routing_key(const char *prefix, const char *suffix);
struct ast_eid *eid_copy(const struct ast_eid *eid);

/*! \brief stasis_amqp configuration */
struct stasis_amqp_conf {
	struct stasis_amqp_global_conf *global;
};

struct app {
	char *name;
};

/*! \brief global config structure */
struct stasis_amqp_global_conf {
	AST_DECLARE_STRING_FIELDS(
		/*! \brief connection name */
		AST_STRING_FIELD(connection);
		/*! \brief exchange name */
		AST_STRING_FIELD(exchange);
	);
	int publish_ami_events;
	int publish_channel_events;
	struct ast_hashtab *exclude_events;
	struct ast_hashtab *include_channelvarset_events;
};

/*! \brief Locking container for safe configuration access. */
static AO2_GLOBAL_OBJ_STATIC(confs);

static struct aco_type global_option = {
	.type = ACO_GLOBAL,
	.name = "global",
	.item_offset = offsetof(struct stasis_amqp_conf, global),
	.category = "^global$",
	.category_match = ACO_WHITELIST,
};

static struct aco_type *global_options[] = ACO_TYPES(&global_option);

static int exclude_events_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct stasis_amqp_global_conf *conf_global = obj;
	char *events = ast_strdupa(var->value);
	char *event_name;

	while ((event_name = ast_strip(strsep(&events, ",")))) {
		if (ast_strlen_zero(event_name)) {
			continue;
		}
		event_name = ast_strdup(event_name);
		ast_hashtab_insert_safe(conf_global->exclude_events, event_name);
	}
	return 0;
}

static int include_channelvarset_events_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct stasis_amqp_global_conf *conf_global = obj;
	char *var_names = ast_strdupa(var->value);
	char *var_name;

	while ((var_name = ast_strip(strsep(&var_names, ",")))) {
		if (ast_strlen_zero(var_name)) {
			continue;
		}
		var_name = ast_strdup(var_name);
		ast_hashtab_insert_safe(conf_global->include_channelvarset_events, var_name);
	}
	return 0;
}

static void conf_global_dtor(void *obj)
{
	struct stasis_amqp_global_conf *global = obj;
	ast_string_field_free_memory(global);
	if (global->exclude_events) {
		ast_hashtab_destroy(global->exclude_events, 0);
	}
	if (global->include_channelvarset_events) {
		ast_hashtab_destroy(global->include_channelvarset_events, 0);
	}

}

static struct stasis_amqp_global_conf *conf_global_create(void)
{
	RAII_VAR(struct stasis_amqp_global_conf *, global, NULL, ao2_cleanup);
	global = ao2_alloc(sizeof(*global), conf_global_dtor);
	if (!global) {
		return NULL;
	}
	if (ast_string_field_init(global, 256) != 0) {
		return NULL;
	}
	global->exclude_events = ast_hashtab_create(
		0,
		ast_hashtab_compare_strings,
		ast_hashtab_resize_tight,
		ast_hashtab_newsize_tight,
		ast_hashtab_hash_string,
		0
	);
	global->include_channelvarset_events = ast_hashtab_create(
		0,
		ast_hashtab_compare_strings,
		ast_hashtab_resize_tight,
		ast_hashtab_newsize_tight,
		ast_hashtab_hash_string,
		0
	);

	aco_set_defaults(&global_option, "global", global);
	return ao2_bump(global);
}


/*! \brief The conf file that's processed for the module. */
static struct aco_file conf_file = {
	/*! The config file name. */
	.filename = CONF_FILENAME,
	/*! The mapping object types to be processed. */
	.types = ACO_TYPES(&global_option),
};

static void conf_dtor(void *obj)
{
	struct stasis_amqp_conf *conf = obj;
	ao2_cleanup(conf->global);
}


static void *conf_alloc(void)
{
	RAII_VAR(struct stasis_amqp_conf *, conf, NULL, ao2_cleanup);
	conf = ao2_alloc_options(sizeof(*conf), conf_dtor,
		AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!conf) {
		return NULL;
	}
	conf->global = conf_global_create();
	if (!conf->global) {
		return NULL;
	}
	return ao2_bump(conf);
}

CONFIG_INFO_STANDARD(cfg_info, confs, conf_alloc,
	.files = ACO_FILES(&conf_file),
	.pre_apply_config = setup_amqp,
);


static int setup_amqp(void)
{
	struct stasis_amqp_conf *conf = aco_pending_config(&cfg_info);
	if (!conf) {
		return 0;
	}
	if (!conf->global) {
		ast_log(LOG_ERROR, "Invalid stasis_amqp.conf\n");
		return -1;
	}
	return 0;
}

static int is_event_excluded(const char *event_name)
{
	RAII_VAR(struct stasis_amqp_conf *, conf, ao2_global_obj_ref(confs), ao2_cleanup);
	const char *ignored = NULL;

	if (ast_hashtab_size(conf->global->exclude_events) == 0) {
		return 0;
	}

	ast_debug(5, "filter on event '%s'\n", event_name);

	ignored = ast_hashtab_lookup(conf->global->exclude_events, event_name);
	if (ignored) {
		ast_debug(5, "ignoring event '%s'\n", event_name);
		return 1;
	}
	return 0;
}

static int is_channelvarset_included(const char *var_name)
{
	RAII_VAR(struct stasis_amqp_conf *, conf, ao2_global_obj_ref(confs), ao2_cleanup);
	const char *included = NULL;

	if (ast_hashtab_size(conf->global->include_channelvarset_events) == 0) {
		return 1;
	}

	ast_debug(5, "processing ChannelVarset filter on variable '%s'\n", var_name);

	if(!var_name) {
		ast_debug(5, "ignoring ChannelVarset with no variable\n");
		return 0;
	}

	included = ast_hashtab_lookup(conf->global->include_channelvarset_events, var_name);
	if (included) {
		ast_debug(5, "including ChannelVarset with variable '%s'\n", var_name);
		return 1;
	}

	ast_debug(5, "ignoring ChannelVarset with variable '%s'\n", var_name);
	return 0;
}

/*!
 * \brief Subscription callback for all channel messages.
 * \param data Data pointer given when creating the subscription.
 * \param sub This subscription.
 * \param topic The topic the message was posted to. This is not necessarily the
 *              topic you subscribed to, since messages may be forwarded between
 *              topics.
 * \param message The message itself.
 */
static void stasis_channel_event_handler(void *data, struct stasis_subscription *sub,
	struct stasis_message *message)
{
	RAII_VAR(struct ast_json *, bus_event, NULL, ast_json_unref);
	RAII_VAR(struct ast_json *, headers, NULL, ast_json_unref);
	RAII_VAR(char *, routing_key, NULL, ast_free);
	struct ast_json *json = NULL;
	const char *event_name = NULL;
	const char *routing_key_prefix = "stasis.channel";
	const char *var_name = NULL;

	if (stasis_subscription_final_message(sub, message)) {
		return;
	}

	if (!(json = stasis_message_to_json(message, NULL))) {
		return;
	}

	if (!(event_name = ast_json_object_string_get(json, "type"))) {
		ast_debug(5, "ignoring stasis event with no type\n");
		ast_json_unref(json);
		return;
	}

	ast_debug(4, "called stasis channel handler for event: '%s'\n", event_name);

	if (is_event_excluded(event_name)) {
		return;
	}

	if (strcmp(event_name, "ChannelVarset") == 0) {
		var_name = ast_strdupa(ast_json_object_string_get(json, "variable"));
		if (!is_channelvarset_included(var_name)) {
			return;
		}
	}

	bus_event = ast_json_pack("{s: s, s: o}", "name", event_name, "data", json);
	if (!bus_event) {
		ast_log(LOG_ERROR, "failed to create json object\n");
		ast_json_unref(json);
		return;
	}

	headers = ast_json_pack("{s: s, s: s}",
			"name", event_name,
			"category", "stasis");
	if (!headers) {
		ast_log(LOG_ERROR, "failed to create AMQP headers\n");
		return;
	}

	if (!(routing_key = new_routing_key(routing_key_prefix, event_name))) {
		ast_log(LOG_ERROR, "failed to create routing key\n");
		return;
	}

	publish_to_amqp(bus_event, headers, routing_key);
}

static int manager_event_to_json(struct ast_json *json, const char *event_name, char *fields)
{
	struct ast_json *json_value = NULL;
	char *line = NULL;
	char *word = NULL;
	char *key, *value;

	if (!(json_value = ast_json_string_create(event_name))) {
		ast_log(LOG_ERROR, "failed to create json string for AMI event name\n");
		return -1;
	}

	if (ast_json_object_set(json, "Event", json_value)) {
		ast_log(LOG_DEBUG, "failed to set json value Event: %s\n", event_name);
		return -1;
	}

	while ((line = strsep(&fields, "\r\n")) != NULL) {
		key = NULL;
		value = NULL;

		while ((word = strsep(&line, ": ")) != NULL) {
			if (!key) {
				key = word;
			} else {
				value = word;
			}
		}

		if (!(json_value = ast_json_string_create(value))) {
			continue;
		}

		if (ast_json_object_set(json, key, json_value)) {
			ast_log(LOG_DEBUG, "failed to set json value %s: %s\n", key, value);
			return -1;
		}
	}

	return 0;
}

static void stasis_app_event_handler(void *data, const char *app_name, struct ast_json *stasis_event)
{
	RAII_VAR(struct ast_json *, bus_event, NULL, ast_json_unref);
	RAII_VAR(struct ast_json *, headers, NULL, ast_json_unref);
	RAII_VAR(char *, routing_key, NULL, ast_free);
	const char *routing_key_prefix = "stasis.app";
	const char *var_name = NULL;

	ast_json_ref(stasis_event);  // Bumping the reference to this event to make sure it stays in memory until we're done

	char *event_name = ast_strdupa(ast_json_object_string_get(stasis_event, "type"));

	if (!event_name) {
		ast_debug(5, "ignoring stasis event with no type\n");
		goto done;
	}

	ast_debug(4, "called stasis app handler for application: '%s' and event: '%s'\n", app_name, event_name);

	if (is_event_excluded(event_name)) {
		goto done;
	}

	if (strcmp(event_name, "ChannelVarset") == 0) {
		var_name = ast_strdupa(ast_json_object_string_get(stasis_event, "variable"));
		if (!is_channelvarset_included(var_name)) {
			goto done;
		}
	}

	if (ast_json_object_set(stasis_event, "application", ast_json_string_create(app_name))) {
		ast_log(LOG_ERROR, "unable to set application item in json");
		goto done;
	};

	bus_event = ast_json_pack("{s: s, s: O, s: s}",
		"name", event_name,
		"data", stasis_event,
		"application", app_name);
	if (!bus_event) {
		ast_log(LOG_ERROR, "failed to create json object\n");
		goto done;
	}

	headers = ast_json_pack("{s: s, s: s, s: s}",
			"name", event_name,
			"category", "stasis",
			"application_name", app_name);
	if (!headers) {
		ast_log(LOG_ERROR, "failed to create AMQP headers\n");
		goto done;
	}

	if (!(routing_key = new_routing_key(routing_key_prefix, app_name))) {
		ast_log(LOG_ERROR, "failed to create routing key\n");
		goto done;
	}

	publish_to_amqp(bus_event, headers, routing_key);

done:
	ast_json_unref(stasis_event);
}


/*!
 * \brief Subscription callback for all AMI messages.
 * \param data Data pointer given when creating the subscription.
 * \param sub This subscription.
 * \param topic The topic the message was posted to. This is not necessarily the
 *              topic you subscribed to, since messages may be forwarded between
 *              topics.
 * \param message The message itself.
 */
static void ami_event_handler(void *data, struct stasis_subscription *sub,
									struct stasis_message *message)
{
	RAII_VAR(struct ast_json *, bus_event, NULL, ast_json_unref);
	RAII_VAR(struct ast_json *, headers, NULL, ast_json_unref);
	RAII_VAR(struct ast_manager_event_blob *, manager_blob, NULL, ao2_cleanup);
	RAII_VAR(char *, fields, NULL, ast_free);
	struct ast_json *event_data = NULL;
	const char *routing_key_prefix = "ami";
	RAII_VAR(char *, routing_key, NULL, ast_free);

	if (!stasis_message_can_be_ami(message)) {
		return;
	}

	if (!(manager_blob = stasis_message_to_ami(message))) {
		/* message has no AMI representation */
		return;
	}

	ast_debug(4, "called ami handler for event: '%s'\n", manager_blob->manager_event);

	if (is_event_excluded(manager_blob->manager_event)) {
		return;
	}

	if (manager_blob->extra_fields) {
		if (!(fields = ast_strdup(manager_blob->extra_fields))) {
			ast_log(LOG_ERROR, "failed to copy AMI event fields\n");
			return;
		}
	}

	if (!(event_data = ast_json_object_create())) {
		ast_log(LOG_ERROR, "failed to create json object\n");
		return;
	}

	if (manager_event_to_json(event_data, manager_blob->manager_event, fields)) {
		ast_log(LOG_ERROR, "failed to create AMI message json payload for %s\n", manager_blob->extra_fields);
		ast_json_unref(event_data);
		return;
	}

	bus_event = ast_json_pack("{s: s, s: o}", "name", manager_blob->manager_event, "data", event_data);
	if (!bus_event) {
		ast_log(LOG_ERROR, "failed to to create json object\n");
		ast_json_unref(event_data);
		return;
	}

	if (!(routing_key = new_routing_key(routing_key_prefix, manager_blob->manager_event))) {
		ast_log(LOG_ERROR, "failed to create routing key\n");
		return;
	}

	headers = ast_json_pack("{s: s, s: s}", "name", manager_blob->manager_event, "category", "ami");
	if (!headers) {
		ast_log(LOG_ERROR, "failed to create AMQP headers\n");
		return;
	}

	publish_to_amqp(bus_event, headers, routing_key);
}

char *new_routing_key(const char *prefix, const char *suffix)
{
	char *ptr = NULL;
	char *routing_key = NULL;
	RAII_VAR(char *, lowered_suffix, NULL, ast_free);
	size_t routing_key_len = strlen(prefix) + strlen(suffix) + 1; /* "prefix.suffix" */

	if (!(lowered_suffix = ast_strdup(suffix))) {
		ast_log(LOG_ERROR, "failed to copy a routing key suffix\n");
		return NULL;
	}

	for (ptr = lowered_suffix; *ptr != '\0'; ptr++) {
		*ptr = tolower(*ptr);
	}

	if (!(routing_key = ast_malloc(routing_key_len + 1))) {
		ast_log(LOG_ERROR, "failed to allocate a string for the routing key\n");
		return NULL;
	}

	if (!(snprintf(routing_key, routing_key_len + 1, "%s.%s", prefix, lowered_suffix))) {
		ast_log(LOG_ERROR, "failed to format the routing key\n");
		return NULL;
	}

	return routing_key;
}


struct ast_eid *eid_copy(const struct ast_eid *eid)
{
	struct ast_eid *new = NULL;
	int i = 0;

	if (!(new = ast_calloc(sizeof(*new), 1))) {
		return NULL;
	}

	for (i = 0; i < 6; i++) {
		new->eid[i] = eid->eid[i];
	}
	return new;
}

static int publish_to_amqp(struct ast_json *body, struct ast_json *headers, const char *routing_key)
{
	RAII_VAR(struct stasis_amqp_conf *, conf, NULL, ao2_cleanup);
	RAII_VAR(struct ast_amqp_connection *, conn, NULL, ao2_cleanup);
	RAII_VAR(char *, msg, NULL, ast_json_free);
	struct ast_json_iter *iter = NULL;
	amqp_table_t *header_table = NULL;
	const char *connection_name = NULL;
	int nb_headers = 0;
	int i = 0;
	int result = 0;

	amqp_basic_properties_t props = {
		._flags = AMQP_BASIC_DELIVERY_MODE_FLAG | AMQP_BASIC_CONTENT_TYPE_FLAG,
		.delivery_mode = 2, /* persistent delivery mode */
		.content_type = amqp_cstring_bytes("application/json")
	};

	conf = ao2_global_obj_ref(confs);
	if (!conf || !conf->global || !conf->global->connection) {
		ast_log(LOG_ERROR, "cannot publish to AMQP without configured connection\n");
		return -1;
	}
	connection_name = conf->global->connection;

	if (!conf->global->exchange) {
		ast_log(LOG_ERROR, "cannot publish to AMQP without a configured exchange\n");
		return -1;
	}

	conn = ast_amqp_get_connection(connection_name);
	if (!conn) {
		ast_log(LOG_ERROR, "Failed to get an AMQP connection for %s\n", connection_name);
		return -1;
	}

	if ((msg = ast_json_dump_string(body)) == NULL) {
		ast_log(LOG_ERROR, "failed to convert json to string\n");
		return -1;
	}

	if (headers) {
		nb_headers = ast_json_object_size(headers);

		if (nb_headers > 0) {
			header_table = &props.headers;
			header_table->num_entries = nb_headers;
			header_table->entries = ast_calloc(nb_headers, sizeof(amqp_table_entry_t));
			for (iter = ast_json_object_iter(headers); iter; iter = ast_json_object_iter_next(headers, iter)) {
				header_table->entries[i].key = amqp_cstring_bytes(ast_json_object_iter_key(iter));
				header_table->entries[i].value.kind = AMQP_FIELD_KIND_UTF8;
				header_table->entries[i].value.value.bytes = amqp_cstring_bytes(ast_json_string_get(ast_json_object_iter_value(iter)));
				i++;
			}
			props._flags |= AMQP_BASIC_HEADERS_FLAG;
		}
	} else {
		props.headers.num_entries = 0;
	}

	if (ast_amqp_basic_publish(
		conn,
		amqp_cstring_bytes(conf->global->exchange),
		amqp_cstring_bytes(routing_key), /* routing key */
		0, /* mandatory; don't return unsendable messages */
		0, /* immediate; allow messages to be queued */
		&props,
		amqp_cstring_bytes(msg))) {
		ast_log(LOG_ERROR, "Error publishing stasis to AMQP\n");
		result = -1;
	}

	if (props.headers.num_entries > 0) {
		ast_free(props.headers.entries);
	}

	return result;
}


static int load_config(int reload)
{
	RAII_VAR(struct stasis_amqp_conf *, conf, NULL, ao2_cleanup);

	if (aco_info_init(&cfg_info) != 0) {
		ast_log(LOG_ERROR, "Failed to initialize config\n");
		aco_info_destroy(&cfg_info);
		return -1;
	}

	aco_option_register(&cfg_info, "connection", ACO_EXACT,
		global_options, "", OPT_STRINGFIELD_T, 0,
		STRFLDSET(struct stasis_amqp_global_conf, connection));
	aco_option_register(&cfg_info, "exchange", ACO_EXACT,
		global_options, "", OPT_STRINGFIELD_T, 0,
		STRFLDSET(struct stasis_amqp_global_conf, exchange));
	aco_option_register(&cfg_info, "publish_ami_events", ACO_EXACT,
		global_options, "yes", OPT_BOOL_T, 1, FLDSET(struct stasis_amqp_global_conf, publish_ami_events));
	aco_option_register(&cfg_info, "publish_channel_events", ACO_EXACT,
		global_options, "yes", OPT_BOOL_T, 1, FLDSET(struct stasis_amqp_global_conf, publish_channel_events));
	aco_option_register_custom(&cfg_info, "exclude_events", ACO_EXACT,
		global_options, "", exclude_events_handler, 0);
	aco_option_register_custom(&cfg_info, "include_channelvarset_events", ACO_EXACT,
		global_options, "", include_channelvarset_events_handler, 0);



	switch (aco_process_config(&cfg_info, reload)) {
	case ACO_PROCESS_ERROR:
		return -1;
	case ACO_PROCESS_OK:
	case ACO_PROCESS_UNCHANGED:
		break;
	}
	conf = ao2_global_obj_ref(confs);
	if (!conf || !conf->global) {
		ast_log(LOG_ERROR, "Error obtaining config from stasis_amqp.conf\n");
		return -1;
	}
	return 0;
}

static int unload_module(void)
{
	RAII_VAR(struct stasis_amqp_conf *, conf, ao2_global_obj_ref(confs), ao2_cleanup);

	if (stasis_app_sched_context) {
		ast_sched_context_destroy(stasis_app_sched_context);
		stasis_app_sched_context = NULL;
	}

	if (conf->global->publish_channel_events) {
		stasis_unsubscribe_and_join(sub);
	}
	if (conf->global->publish_ami_events) {
		stasis_unsubscribe_and_join(manager);
	}

	return 0;
}

int ast_subscribe_to_stasis(const char *app_name)
{
	int res = 0;
	ast_debug(1, "called subscribe to stasis for application: '%s'\n", app_name);
	res = stasis_app_register(app_name, &stasis_app_event_handler, NULL);
	return res;
}

int ast_unsubscribe_from_stasis(const char *app_name)
{
	ast_debug(1, "called unsubscribe from stasis for application: '%s'\n", app_name);
	stasis_app_unregister(app_name);
	return 0;
}

static int load_module(void)
{
	RAII_VAR(struct stasis_amqp_conf *, conf, NULL, ao2_cleanup);

	if (load_config(0) != 0) {
		ast_log(LOG_WARNING, "Configuration failed to load\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	conf = ao2_global_obj_ref(confs);

	if (conf->global->publish_ami_events) {
		ast_debug(3, "subscribing to AMI events\n");
		/* Subscription to receive all of the messages from manager topic */
		if (!(manager = stasis_subscribe(ast_manager_get_topic(), ami_event_handler, NULL))) {
			return AST_MODULE_LOAD_DECLINE;
		}
	}

	if (!(stasis_app_sched_context = ast_sched_context_create())) {
		ast_log(LOG_ERROR, "failed to create scheduler context\n");
		if (conf->global->publish_ami_events) {
			stasis_unsubscribe_and_join(manager);
		}
		return AST_MODULE_LOAD_DECLINE;
	}

	if (conf->global->publish_channel_events) {
		ast_debug(3, "subscribing to channel events\n");
		/* Subscription to receive all of the messages from channel topic */
		if (!(sub = stasis_subscribe(ast_channel_topic_all(), stasis_channel_event_handler, NULL))) {
			if (conf->global->publish_ami_events) {
				stasis_unsubscribe_and_join(manager);
			}
			return AST_MODULE_LOAD_DECLINE;
		}
	}

	if (ast_sched_start_thread(stasis_app_sched_context)) {
		ast_log(LOG_ERROR, "failed to start scheduler thread\n");
		if (conf->global->publish_ami_events) {
			stasis_unsubscribe_and_join(manager);
		}
		if (conf->global->publish_channel_events) {
			stasis_unsubscribe_and_join(sub);
		}
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS | AST_MODFLAG_LOAD_ORDER, "Send all Stasis messages to AMQP",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_APP_DEPEND,
	.requires = "res_stasis,res_amqp",
);
