/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2013, Digium, Inc.
 *
 * Matt Jordan <mjordan@digium.com>
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

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>
#include <pjlib.h>

#include "asterisk/res_pjsip.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/astobj2.h"
#include "asterisk/cli.h"
#include "asterisk/time.h"
#include "asterisk/test.h"
#include "asterisk/statsd.h"
#include "include/res_pjsip_private.h"
#include "asterisk/taskprocessor.h"

#define DEFAULT_LANGUAGE "en"
#define DEFAULT_ENCODING "text/plain"
#define QUALIFIED_BUCKETS 211

static const char *status_map [] = {
	[UNAVAILABLE] = "Unreachable",
	[AVAILABLE] = "Reachable",
	[UNKNOWN] = "Unknown",
	[CREATED] = "Created",
	[REMOVED] = "Removed",
};

static const char *short_status_map [] = {
	[UNAVAILABLE] = "Unavail",
	[AVAILABLE] = "Avail",
	[UNKNOWN] = "Unknown",
	[CREATED] = "Created",
	[REMOVED] = "Removed",
};

static void contact_deleted(const void *obj);
static void qualify_and_schedule(struct ast_sip_contact *contact);

const char *ast_sip_get_contact_status_label(const enum ast_sip_contact_status_type status)
{
	return status_map[status];
}

const char *ast_sip_get_contact_short_status_label(const enum ast_sip_contact_status_type status)
{
	return short_status_map[status];
}

/*!
 * \internal
 * \brief Destroy a ast_sip_contact_status object.
 */
static void contact_status_destroy(void * obj)
{
	struct ast_sip_contact_status *status = obj;

	ast_string_field_free_memory(status);
}

/*!
 * \internal
 * \brief Create a ast_sip_contact_status object.
 */
static void *contact_status_alloc(const char *name)
{
	struct ast_sip_contact_status *status = ast_sorcery_generic_alloc(sizeof(*status), contact_status_destroy);
	char *id = ast_strdupa(name);
	char *aor = id;
	char *aor_separator = NULL;

	if (!status) {
		ast_log(LOG_ERROR, "Unable to allocate ast_sip_contact_status\n");
		return NULL;
	}

	if (ast_string_field_init(status, 256)) {
		ast_log(LOG_ERROR, "Unable to allocate ast_sip_contact_status stringfields\n");
		ao2_cleanup(status);
		return NULL;
	}

	/* Dynamic contacts are delimited with ";@" and static ones with "@@" */
	if ((aor_separator = strstr(id, ";@")) || (aor_separator = strstr(id, "@@"))) {
		*aor_separator = '\0';
	}
	ast_assert(aor_separator != NULL);

	ast_string_field_set(status, aor, aor);
	status->status = CREATED;

	return status;
}

static int qualify_and_schedule_aor_contact(void *obj)
{
	struct ast_sip_contact *contact = obj;
	struct ast_sip_aor *aor;

	if (!contact || ast_strlen_zero(contact->aor) ||
		!(aor = ast_sip_location_retrieve_aor(contact->aor))) {
		ao2_ref(contact, -1);
		return -1;
	}

	contact->qualify_frequency = aor->qualify_frequency;
	contact->qualify_timeout = aor->qualify_timeout;
	contact->authenticate_qualify = aor->authenticate_qualify;

	ao2_ref(aor, -1);

	qualify_and_schedule(contact);
	ao2_ref(contact, -1);

	return 0;
}

AST_MUTEX_DEFINE_STATIC(creation_lock);

/*!
 * \brief Retrieve a ast_sip_contact_status object from sorcery creating
 *        one if not found.
 */
struct ast_sip_contact_status *ast_res_pjsip_find_or_create_contact_status(const struct ast_sip_contact *contact)
{
	struct ast_sip_contact_status *status;
	SCOPED_MUTEX(lock, &creation_lock);

	status = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), CONTACT_STATUS,
		ast_sorcery_object_get_id(contact));
	if (status) {
		return status;
	}

	status = ast_sorcery_alloc(ast_sip_get_sorcery(), CONTACT_STATUS,
		ast_sorcery_object_get_id(contact));
	if (!status) {
		ast_log(LOG_ERROR, "Unable to create ast_sip_contact_status for contact %s/%s\n",
			contact->aor, contact->uri);
		return NULL;
	}

	ast_string_field_set(status, uri, contact->uri);
	status->rtt_start = ast_tv(0, 0);
	status->rtt = 0;

	if (ast_sorcery_create(ast_sip_get_sorcery(), status)) {
		ast_log(LOG_ERROR, "Unable to persist ast_sip_contact_status for contact %s\n",
			contact->uri);
		ao2_ref(status, -1);
		return NULL;
	}

	/* The permanent contact added after asterisk start should be qualified. */
	if (ast_test_flag(&ast_options, AST_OPT_FLAG_FULLY_BOOTED) && ast_tvzero(contact->expiration_time)) {
		/*
		 * The FULLY_BOOTED to filter out contacts that already existed when asterisk started.
		 * The zero expiration_time to select only permanent contacts.
		 */
		ao2_ref((struct ast_sip_contact *) contact, +1);
		if (ast_sip_push_task(NULL, qualify_and_schedule_aor_contact, (struct ast_sip_contact *) contact)) {
			ao2_ref((struct ast_sip_contact *) contact, -1);
		}
	}

	ast_statsd_log_string_va("PJSIP.contacts.states.%s", AST_STATSD_GAUGE,
		"+1", 1.0, ast_sip_get_contact_status_label(status->status));

	return status;
}

/*!
 * \internal
 * \brief Update an ast_sip_contact_status's elements.
 */
static void update_contact_status(const struct ast_sip_contact *contact,
	enum ast_sip_contact_status_type value, int is_contact_refresh)
{
	RAII_VAR(struct ast_sip_contact_status *, status, NULL, ao2_cleanup);
	RAII_VAR(struct ast_sip_contact_status *, update, NULL, ao2_cleanup);

	status = ast_res_pjsip_find_or_create_contact_status(contact);
	if (!status) {
		ast_log(LOG_ERROR, "Unable to find ast_sip_contact_status for contact %s\n",
			contact->uri);
		return;
	}

	if (is_contact_refresh
		&& status->status == CREATED) {
		/*
		 * The contact status hasn't been updated since creation
		 * and we don't want to re-send a created status.
		 */
		if (contact->qualify_frequency
			|| status->rtt_start.tv_sec > 0) {
			/* Ignore, the status will change soon. */
			return;
		}

		/*
		 * Convert to a regular contact status update
		 * because the status may never change.
		 */
		is_contact_refresh = 0;
		value = UNKNOWN;
	}

	update = ast_sorcery_alloc(ast_sip_get_sorcery(), CONTACT_STATUS,
		ast_sorcery_object_get_id(status));
	if (!update) {
		ast_log(LOG_ERROR, "Unable to allocate ast_sip_contact_status for contact %s\n",
			contact->uri);
		return;
	}

	ast_string_field_set(update, uri, contact->uri);

	if (is_contact_refresh) {
		/* Copy everything just to set the refresh flag. */
		update->status = status->status;
		update->last_status = status->last_status;
		update->rtt = status->rtt;
		update->rtt_start = status->rtt_start;
		update->refresh = 1;
	} else {
		update->last_status = status->status;
		update->status = value;

		/*
		 * if the contact is available calculate the rtt as
		 * the diff between the last start time and "now"
		 */
		update->rtt = update->status == AVAILABLE && status->rtt_start.tv_sec > 0
			? ast_tvdiff_us(ast_tvnow(), status->rtt_start)
			: 0;
		update->rtt_start = ast_tv(0, 0);

		ast_test_suite_event_notify("AOR_CONTACT_QUALIFY_RESULT",
			"Contact: %s\r\n"
			"Status: %s\r\n"
			"RTT: %" PRId64,
			ast_sorcery_object_get_id(update),
			ast_sip_get_contact_status_label(update->status),
			update->rtt);
	}

	if (ast_sorcery_update(ast_sip_get_sorcery(), update)) {
		ast_log(LOG_ERROR, "Unable to update ast_sip_contact_status for contact %s\n",
			contact->uri);
	}
}

/*!
 * \internal
 * \brief Initialize the start time on a contact status so the round
 *        trip time can be calculated upon a valid response.
 */
static void init_start_time(const struct ast_sip_contact *contact)
{
	RAII_VAR(struct ast_sip_contact_status *, status, NULL, ao2_cleanup);
	RAII_VAR(struct ast_sip_contact_status *, update, NULL, ao2_cleanup);

	status = ast_res_pjsip_find_or_create_contact_status(contact);
	if (!status) {
		ast_log(LOG_ERROR, "Unable to find ast_sip_contact_status for contact %s\n",
			contact->uri);
		return;
	}

	update = ast_sorcery_alloc(ast_sip_get_sorcery(), CONTACT_STATUS,
		ast_sorcery_object_get_id(status));
	if (!update) {
		ast_log(LOG_ERROR, "Unable to copy ast_sip_contact_status for contact %s\n",
			contact->uri);
		return;
	}

	ast_string_field_set(status, uri, contact->uri);
	update->status = status->status;
	update->last_status = status->last_status;
	update->rtt = status->rtt;
	update->rtt_start = ast_tvnow();

	if (ast_sorcery_update(ast_sip_get_sorcery(), update)) {
		ast_log(LOG_ERROR, "Unable to update ast_sip_contact_status for contact %s\n",
			contact->uri);
	}
}

/*!
 * \internal
 * \brief For an endpoint try to match the given contact->aor.
 */
static int on_endpoint(void *obj, void *arg, int flags)
{
	struct ast_sip_endpoint *endpoint = obj;
	char *contact_aor = arg;
	char *aor_name;
	char *aors;

	if (!arg || ast_strlen_zero(endpoint->aors)) {
		return 0;
	}

	aors = ast_strdupa(endpoint->aors);
	while ((aor_name = ast_strip(strsep(&aors, ",")))) {
		if (!strcmp(contact_aor, aor_name)) {
			return CMP_MATCH;
		}
	}

	return 0;
}

/*!
 * \internal
 * \brief Find an endpoint associated with the given contact.
 */
static struct ast_sip_endpoint *find_an_endpoint(struct ast_sip_contact *contact)
{
	struct ao2_container *endpoints;
	struct ast_sip_endpoint *endpoint;
	struct ast_variable *var;
	char *aor = ast_alloca(strlen(contact->aor) + 3);

	sprintf(aor, "%%%s%%", contact->aor);
	var = ast_variable_new("aors LIKE", aor, "");
	endpoints = ast_sorcery_retrieve_by_fields(ast_sip_get_sorcery(),
		"endpoint", AST_RETRIEVE_FLAG_MULTIPLE, var);

	ast_variables_destroy(var);

	/*
	 * Because aors are a string list, we have to use a pattern match but since a simple
	 * pattern match could return an endpoint that has an aor of "aaabccc" when searching
	 * for "abc", we still have to iterate over them to find an exact aor match.
	 */
	endpoint = ao2_callback(endpoints, 0, on_endpoint, (char *)contact->aor);
	ao2_ref(endpoints, -1);

	return endpoint;
}

/*!
 * \internal
 * \brief Receive a response to the qualify contact request.
 */
static void qualify_contact_cb(void *token, pjsip_event *e)
{
	struct ast_sip_contact *contact = token;

	switch(e->body.tsx_state.type) {
	default:
		ast_log(LOG_ERROR, "Unexpected PJSIP event %u\n", e->body.tsx_state.type);
		/* Fall through */
	case PJSIP_EVENT_TRANSPORT_ERROR:
	case PJSIP_EVENT_TIMER:
		update_contact_status(contact, UNAVAILABLE, 0);
		break;
	case PJSIP_EVENT_RX_MSG:
		update_contact_status(contact, AVAILABLE, 0);
		break;
	}
	ao2_cleanup(contact);
}

/*!
 * \internal
 * \brief Attempt to qualify the contact
 *
 * \details Sends a SIP OPTIONS request to the given contact in order to make
 *         sure that contact is available.
 */
static int qualify_contact(struct ast_sip_endpoint *endpoint, struct ast_sip_contact *contact)
{
	pjsip_tx_data *tdata;
	RAII_VAR(struct ast_sip_endpoint *, endpoint_local, NULL, ao2_cleanup);

	if (endpoint) {
		endpoint_local = ao2_bump(endpoint);
	} else {
		if (!ast_strlen_zero(contact->endpoint_name)) {
			endpoint_local = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), "endpoint", contact->endpoint_name);
		}
		if (!endpoint_local) {
			endpoint_local = find_an_endpoint(contact);
		}
		if (!endpoint_local) {
			ast_log(LOG_WARNING, "Unable to find an endpoint to qualify contact %s. Deleting this contact\n",
				contact->uri);
			contact_deleted(contact);
			return -1;
		}
	}

	if (ast_sip_create_request("OPTIONS", NULL, endpoint_local, NULL, contact, &tdata)) {
		ast_log(LOG_ERROR, "Unable to create request to qualify contact %s\n",
			contact->uri);
		return -1;
	}

	/* If an outbound proxy is specified set it on this request */
	if (!ast_strlen_zero(contact->outbound_proxy) &&
		ast_sip_set_outbound_proxy(tdata, contact->outbound_proxy)) {
		pjsip_tx_data_dec_ref(tdata);
		ast_log(LOG_ERROR, "Unable to apply outbound proxy on request to qualify contact %s\n",
			contact->uri);
		return -1;
	}

	init_start_time(contact);

	ao2_ref(contact, +1);
	if (ast_sip_send_out_of_dialog_request(tdata, endpoint_local, (int)(contact->qualify_timeout * 1000), contact, qualify_contact_cb)
		!= PJ_SUCCESS) {
		ast_log(LOG_ERROR, "Unable to send request to qualify contact %s\n",
			contact->uri);
		update_contact_status(contact, UNAVAILABLE, 0);
		ao2_ref(contact, -1);
		return -1;
	}

	return 0;
}

/*!
 * \internal
 * \brief Scheduling context for sending QUALIFY request at specified intervals.
 */
static struct ast_sched_context *sched;

/*!
 * \internal
 * \brief Container to hold all actively scheduled qualifies.
 */
static struct ao2_container *sched_qualifies;

/*!
 * \internal
 * \brief Structure to hold qualify contact scheduling information.
 */
struct sched_data {
	/*! The scheduling id */
	int id;
	/*! The the contact being checked */
	struct ast_sip_contact *contact;
};

/*!
 * \internal
 * \brief Destroy the scheduled data and remove from scheduler.
 */
static void sched_data_destructor(void *obj)
{
	struct sched_data *data = obj;

	ao2_cleanup(data->contact);
}
/*!
 * \internal
 * \brief Create the scheduling data object.
 */
static struct sched_data *sched_data_create(struct ast_sip_contact *contact)
{
	struct sched_data *data;

	data = ao2_t_alloc(sizeof(*data), sched_data_destructor, contact->uri);
	if (!data) {
		ast_log(LOG_ERROR, "Unable to create schedule qualify data for contact %s\n",
			contact->uri);
		return NULL;
	}

	data->contact = contact;
	ao2_ref(data->contact, +1);

	return data;
}

/*!
 * \internal
 * \brief Send a qualify contact request within a threaded task.
 */
static int qualify_contact_task(void *obj)
{
	struct ast_sip_contact *contact = obj;
	int res;

	res = qualify_contact(NULL, contact);
	ao2_ref(contact, -1);
	return res;
}

/*!
 * \internal
 * \brief Send a scheduled qualify contact request.
 */
static int qualify_contact_sched(const void *obj)
{
	struct sched_data *data = (struct sched_data *) obj;
	struct ast_sip_aor *aor;

	/* This helps us to determine if an AOR has been removed
	 * from configuration, and if so, stop qualifying the
	 * contact
	 */
	aor = ast_sip_location_retrieve_aor(data->contact->aor);
	if (!aor) {
		ao2_ref(data, -1);
		return 0;
	}
	ao2_ref(aor, -1);

	ao2_ref(data->contact, +1);
	if (ast_sip_push_task(NULL, qualify_contact_task, data->contact)) {
		ao2_ref(data->contact, -1);
	}

	/*
	 * Always reschedule rather than have a potential race cleaning
	 * up the data object ref between self deletion and an external
	 * deletion.
	 */
	return data->contact->qualify_frequency * 1000;
}

/*!
 * \internal
 * \brief Set up a scheduled qualify contact check.
 */
static void schedule_qualify(struct ast_sip_contact *contact, int initial_interval)
{
	struct sched_data *data;

	data = sched_data_create(contact);
	if (!data) {
		return;
	}

	ast_assert(contact->qualify_frequency != 0);

	ao2_t_ref(data, +1, "Ref for qualify_contact_sched() scheduler entry");
	data->id = ast_sched_add_variable(sched, initial_interval,
		qualify_contact_sched, data, 1);
	if (data->id < 0) {
		ao2_t_ref(data, -1, "Cleanup failed scheduler add");
		ast_log(LOG_ERROR, "Unable to schedule qualify for contact %s\n",
			contact->uri);
	} else if (!ao2_link(sched_qualifies, data)) {
		AST_SCHED_DEL_UNREF(sched, data->id,
			ao2_t_ref(data, -1, "Cleanup scheduler for failed ao2_link"));
	}
	ao2_t_ref(data, -1, "Done setting up scheduler entry");
}

/*!
 * \internal
 * \brief Remove the contact from the scheduler.
 */
static void unschedule_qualify(struct ast_sip_contact *contact)
{
	struct sched_data *data;

	data = ao2_find(sched_qualifies, contact, OBJ_UNLINK | OBJ_SEARCH_KEY);
	if (!data) {
		return;
	}

	AST_SCHED_DEL_UNREF(sched, data->id,
		ao2_t_ref(data, -1, "Delete scheduler entry ref"));
	ao2_t_ref(data, -1, "Done with ao2_find ref");
}

/*!
 * \internal
 * \brief Qualify the given contact and set up scheduling if configured.
 */
static void qualify_and_schedule(struct ast_sip_contact *contact)
{
	unschedule_qualify(contact);

	if (contact->qualify_frequency) {
		ao2_ref(contact, +1);
		if (ast_sip_push_task(NULL, qualify_contact_task, contact)) {
			ao2_ref(contact, -1);
		}

		schedule_qualify(contact, contact->qualify_frequency * 1000);
	} else {
		update_contact_status(contact, UNKNOWN, 0);
	}
}

/*!
 * \internal
 * \brief A new contact has been created make sure it is available.
 */
static void contact_created(const void *obj)
{
	qualify_and_schedule((struct ast_sip_contact *) obj);
}

/*!
 * \internal
 * \brief A contact has been updated.
 */
static void contact_updated(const void *obj)
{
	update_contact_status(obj, AVAILABLE, 1);
}

/*!
 * \internal
 * \brief A contact has been deleted remove status tracking.
 */
static void contact_deleted(const void *obj)
{
	struct ast_sip_contact *contact = (struct ast_sip_contact *) obj;
	struct ast_sip_contact_status *status;

	unschedule_qualify(contact);

	status = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), CONTACT_STATUS,
		ast_sorcery_object_get_id(contact));
	if (!status) {
		return;
	}

	if (ast_sorcery_delete(ast_sip_get_sorcery(), status)) {
		ast_log(LOG_ERROR, "Unable to delete ast_sip_contact_status for contact %s\n",
			contact->uri);
	}
	ao2_ref(status, -1);
}

static const struct ast_sorcery_observer contact_observer = {
	.created = contact_created,
	.updated = contact_updated,
	.deleted = contact_deleted,
};

static pj_bool_t options_start(void)
{
	sched = ast_sched_context_create();
	if (!sched) {
		return -1;
	}
	if (ast_sched_start_thread(sched)) {
		ast_sched_context_destroy(sched);
		sched = NULL;
		return -1;
	}

	if (ast_sorcery_observer_add(ast_sip_get_sorcery(), "contact", &contact_observer)) {
		ast_log(LOG_WARNING, "Unable to add contact observer\n");
		ast_sched_context_destroy(sched);
		sched = NULL;
		return -1;
	}

	return PJ_SUCCESS;
}

static int sched_qualifies_empty(void *obj, void *arg, int flags)
{
	ao2_t_ref(obj, -1, "Release ref held by destroyed scheduler context.");
	return CMP_MATCH;
}

static pj_bool_t options_stop(void)
{
	ast_sorcery_observer_remove(ast_sip_get_sorcery(), "contact", &contact_observer);

	if (sched) {
		ast_sched_context_destroy(sched);
		sched = NULL;
	}

	/* Empty the container of scheduling data refs. */
	ao2_callback(sched_qualifies, OBJ_UNLINK | OBJ_NODATA | OBJ_MULTIPLE,
		sched_qualifies_empty, NULL);

	return PJ_SUCCESS;
}

static pj_status_t send_options_response(pjsip_rx_data *rdata, int code)
{
	pjsip_endpoint *endpt = ast_sip_get_pjsip_endpoint();
	pjsip_dialog *dlg = pjsip_rdata_get_dlg(rdata);
	pjsip_transaction *trans = pjsip_rdata_get_tsx(rdata);
	pjsip_tx_data *tdata;
	const pjsip_hdr *hdr;
	pj_status_t status;

	/* Make the response object */
	status = ast_sip_create_response(rdata, code, NULL, &tdata);
	if (status != PJ_SUCCESS) {
		ast_log(LOG_ERROR, "Unable to create response (%d)\n", status);
		return status;
	}

	/* Add appropriate headers */
	if ((hdr = pjsip_endpt_get_capability(endpt, PJSIP_H_ACCEPT, NULL))) {
		pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)pjsip_hdr_clone(tdata->pool, hdr));
	}
	if ((hdr = pjsip_endpt_get_capability(endpt, PJSIP_H_ALLOW, NULL))) {
		pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)pjsip_hdr_clone(tdata->pool, hdr));
	}
	if ((hdr = pjsip_endpt_get_capability(endpt, PJSIP_H_SUPPORTED, NULL))) {
		pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)pjsip_hdr_clone(tdata->pool, hdr));
	}

	/*
	 * XXX TODO: pjsip doesn't care a lot about either of these headers -
	 * while it provides specific methods to create them, they are defined
	 * to be the standard string header creation. We never did add them
	 * in chan_sip, although RFC 3261 says they SHOULD. Hard coded here.
	 */
	ast_sip_add_header(tdata, "Accept-Encoding", DEFAULT_ENCODING);
	ast_sip_add_header(tdata, "Accept-Language", DEFAULT_LANGUAGE);

	if (dlg && trans) {
		status = pjsip_dlg_send_response(dlg, trans, tdata);
	} else {
		struct ast_sip_endpoint *endpoint;

		endpoint = ast_pjsip_rdata_get_endpoint(rdata);
		status = ast_sip_send_stateful_response(rdata, tdata, endpoint);
		ao2_cleanup(endpoint);
	}

	if (status != PJ_SUCCESS) {
		ast_log(LOG_ERROR, "Unable to send response (%d)\n", status);
	}

	return status;
}

static pj_bool_t options_on_rx_request(pjsip_rx_data *rdata)
{
	RAII_VAR(struct ast_sip_endpoint *, endpoint, NULL, ao2_cleanup);
	pjsip_uri *ruri;
	pjsip_sip_uri *sip_ruri;
	char exten[AST_MAX_EXTENSION];

	if (pjsip_method_cmp(&rdata->msg_info.msg->line.req.method, &pjsip_options_method)) {
		return PJ_FALSE;
	}

	if (!(endpoint = ast_pjsip_rdata_get_endpoint(rdata))) {
		return PJ_FALSE;
	}

	ruri = rdata->msg_info.msg->line.req.uri;
	if (!PJSIP_URI_SCHEME_IS_SIP(ruri) && !PJSIP_URI_SCHEME_IS_SIPS(ruri)) {
		send_options_response(rdata, 416);
		return PJ_TRUE;
	}

	sip_ruri = pjsip_uri_get_uri(ruri);
	ast_copy_pj_str(exten, &sip_ruri->user, sizeof(exten));

	/*
	 * We may want to match in the dialplan without any user
	 * options getting in the way.
	 */
	AST_SIP_USER_OPTIONS_TRUNCATE_CHECK(exten);

	if (ast_shutting_down()) {
		/*
		 * Not taking any new calls at this time.
		 * Likely a server availability OPTIONS poll.
		 */
		send_options_response(rdata, 503);
	} else if (!ast_strlen_zero(exten)
		&& !ast_exists_extension(NULL, endpoint->context, exten, 1, NULL)) {
		send_options_response(rdata, 404);
	} else {
		send_options_response(rdata, 200);
	}
	return PJ_TRUE;
}

static pjsip_module options_module = {
	.name = {"Options Module", 14},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_APPLICATION,
	.start = options_start,
	.stop = options_stop,
	.on_rx_request = options_on_rx_request,
};

/*!
 * \internal
 * \brief Send qualify request to the given contact.
 */
static int cli_on_contact(void *obj, void *arg, void *data, int flags)
{
	struct ast_sip_contact *contact = obj;
	struct ast_sip_endpoint *endpoint = data;
	int *cli_fd = arg;

	ast_cli(*cli_fd, " contact %s\n", contact->uri);
	qualify_contact(endpoint, contact);
	return 0;
}

/*!
 * \brief Data pushed to threadpool to qualify endpoints from the CLI
 */
struct qualify_data {
	/*! Endpoint that is being qualified */
	struct ast_sip_endpoint *endpoint;
	/*! CLI File descriptor for printing messages */
	int cli_fd;
};

static struct qualify_data *qualify_data_alloc(struct ast_sip_endpoint *endpoint, int cli_fd)
{
	struct qualify_data *qual_data;

	qual_data = ast_malloc(sizeof(*qual_data));
	if (!qual_data) {
		return NULL;
	}

	qual_data->endpoint = ao2_bump(endpoint);
	qual_data->cli_fd = cli_fd;
	return qual_data;
}

static void qualify_data_destroy(struct qualify_data *qual_data)
{
	ao2_cleanup(qual_data->endpoint);
	ast_free(qual_data);
}

/*!
 * \internal
 * \brief For an endpoint iterate over and qualify all aors/contacts
 */
static int cli_qualify_contacts(void *data)
{
	char *aors;
	char *aor_name;
	RAII_VAR(struct qualify_data *, qual_data, data, qualify_data_destroy);
	struct ast_sip_endpoint *endpoint = qual_data->endpoint;
	int cli_fd = qual_data->cli_fd;
	const char *endpoint_name = ast_sorcery_object_get_id(endpoint);

	if (ast_strlen_zero(endpoint->aors)) {
		ast_cli(cli_fd, "Endpoint %s has no AoR's configured\n",
			endpoint_name);
		return 0;
	}

	aors = ast_strdupa(endpoint->aors);
	while ((aor_name = ast_strip(strsep(&aors, ",")))) {
		struct ast_sip_aor *aor;
		struct ao2_container *contacts;

		aor = ast_sip_location_retrieve_aor(aor_name);
		if (!aor) {
			continue;
		}

		contacts = ast_sip_location_retrieve_aor_contacts(aor);
		if (contacts) {
			ast_cli(cli_fd, "Sending qualify to endpoint %s\n", endpoint_name);
			ao2_callback_data(contacts, OBJ_NODATA, cli_on_contact, &cli_fd, endpoint);
			ao2_ref(contacts, -1);
		}

		ao2_ref(aor, -1);
	}
	return 0;
}

static char *cli_qualify(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	RAII_VAR(struct ast_sip_endpoint *, endpoint, NULL, ao2_cleanup);
	const char *endpoint_name;
	struct qualify_data *qual_data;

	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip qualify";
		e->usage =
			"Usage: pjsip qualify <endpoint>\n"
			"       Send a SIP OPTIONS request to all contacts on the endpoint.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3) {
		return CLI_SHOWUSAGE;
	}

	endpoint_name = a->argv[2];

	if (!(endpoint = ast_sorcery_retrieve_by_id(
		      ast_sip_get_sorcery(), "endpoint", endpoint_name))) {
		ast_cli(a->fd, "Unable to retrieve endpoint %s\n", endpoint_name);
		return CLI_FAILURE;
	}

	qual_data = qualify_data_alloc(endpoint, a->fd);
	if (!qual_data) {
		return CLI_FAILURE;
	}

	if (ast_sip_push_task(NULL, cli_qualify_contacts, qual_data)) {
		qualify_data_destroy(qual_data);
		return CLI_FAILURE;
	}

	return CLI_SUCCESS;
}

/*!
 * \internal
 * \brief Send qualify request to the given contact.
 */
static int ami_contact_cb(void *obj, void *arg, int flags)
{
	struct ast_sip_contact *contact = obj;

	ao2_ref(contact, +1);
	if (ast_sip_push_task(NULL, qualify_contact_task, contact)) {
		ao2_ref(contact, -1);
	}
	return 0;
}

static int ami_sip_qualify(struct mansession *s, const struct message *m)
{
	const char *endpoint_name = astman_get_header(m, "Endpoint");
	RAII_VAR(struct ast_sip_endpoint *, endpoint, NULL, ao2_cleanup);
	char *aors;
	char *aor_name;

	if (ast_strlen_zero(endpoint_name)) {
		astman_send_error(s, m, "Endpoint parameter missing.");
		return 0;
	}

	endpoint = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), "endpoint",
		endpoint_name);
	if (!endpoint) {
		astman_send_error(s, m, "Unable to retrieve endpoint\n");
		return 0;
	}

	/* send a qualify for all contacts registered with the endpoint */
	if (ast_strlen_zero(endpoint->aors)) {
		astman_send_error(s, m, "No AoRs configured for endpoint\n");
		return 0;
	}

	aors = ast_strdupa(endpoint->aors);
	while ((aor_name = ast_strip(strsep(&aors, ",")))) {
		struct ast_sip_aor *aor;
		struct ao2_container *contacts;

		aor = ast_sip_location_retrieve_aor(aor_name);
		if (!aor) {
			continue;
		}

		contacts = ast_sip_location_retrieve_aor_contacts(aor);
		if (contacts) {
			ao2_callback(contacts, OBJ_NODATA, ami_contact_cb, NULL);
			ao2_ref(contacts, -1);
		}

		ao2_ref(aor, -1);
	}

	astman_send_ack(s, m, "Endpoint found, will qualify");
	return 0;
}

static struct ast_cli_entry cli_options[] = {
	AST_CLI_DEFINE(cli_qualify, "Send an OPTIONS request to a PJSIP endpoint")
};

static int sched_qualifies_hash_fn(const void *obj, int flags)
{
	const struct sched_data *object;
	const struct ast_sip_contact *key;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_KEY:
		key = obj;
		break;
	case OBJ_SEARCH_OBJECT:
		object = obj;
		key = object->contact;
		break;
	default:
		/* Hash can only work on something with a full key. */
		ast_assert(0);
		return 0;
	}
	return ast_str_hash(ast_sorcery_object_get_id(key));
}

static int sched_qualifies_cmp_fn(void *obj, void *arg, int flags)
{
	const struct sched_data *object_left = obj;
	const struct sched_data *object_right = arg;
	struct ast_sip_contact *right_key = arg;
	int cmp;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_OBJECT:
		right_key = object_right->contact;
		/* Fall through */
	case OBJ_SEARCH_KEY:
		cmp = strcmp(ast_sorcery_object_get_id(object_left->contact),
			ast_sorcery_object_get_id(right_key));
		break;
	case OBJ_SEARCH_PARTIAL_KEY:
		/* Not supported by container. */
		ast_assert(0);
		return 0;
	default:
		/*
		 * What arg points to is specific to this traversal callback
		 * and has no special meaning to astobj2.
		 */
		cmp = 0;
		break;
	}
	if (cmp) {
		return 0;
	}
	/*
	 * At this point the traversal callback is identical to a sorted
	 * container.
	 */
	return CMP_MATCH;
}

static int rtt_start_handler(const struct aco_option *opt,
	struct ast_variable *var, void *obj)
{
	struct ast_sip_contact_status *status = obj;
	long int sec, usec;

	if (sscanf(var->value, "%ld.%06ld", &sec, &usec) != 2) {
		return -1;
	}

	status->rtt_start = ast_tv(sec, usec);

	return 0;
}

static int rtt_start_to_str(const void *obj, const intptr_t *args, char **buf)
{
	const struct ast_sip_contact_status *status = obj;

	if (ast_asprintf(buf, "%ld.%06ld", (long)status->rtt_start.tv_sec, (long)status->rtt_start.tv_usec) == -1) {
		return -1;
	}

	return 0;
}

static char status_value_unknown[2];
static char status_value_created[2];

int ast_sip_initialize_sorcery_qualify(void)
{
	struct ast_sorcery *sorcery = ast_sip_get_sorcery();

	/* initialize sorcery ast_sip_contact_status resource */
	ast_sorcery_apply_default(sorcery, CONTACT_STATUS, "memory", NULL);
	ast_sorcery_object_set_congestion_levels(sorcery, CONTACT_STATUS, -1,
		3 * AST_TASKPROCESSOR_HIGH_WATER_LEVEL);

	if (ast_sorcery_internal_object_register(sorcery, CONTACT_STATUS,
					contact_status_alloc, NULL, NULL)) {
		ast_log(LOG_ERROR, "Unable to register ast_sip_contact_status in sorcery\n");
		return -1;
	}

	snprintf(status_value_unknown, sizeof(status_value_unknown), "%u", UNKNOWN);
	ast_sorcery_object_field_register_nodoc(sorcery, CONTACT_STATUS, "last_status",
		status_value_unknown, OPT_UINT_T, 0, FLDSET(struct ast_sip_contact_status, last_status));
	snprintf(status_value_created, sizeof(status_value_created), "%u", CREATED);
	ast_sorcery_object_field_register_nodoc(sorcery, CONTACT_STATUS, "status",
		status_value_created, OPT_UINT_T, 0, FLDSET(struct ast_sip_contact_status, status));
	ast_sorcery_object_field_register_custom_nodoc(sorcery, CONTACT_STATUS, "rtt_start",
		"0.0", rtt_start_handler, rtt_start_to_str, NULL, 0, 0);
	ast_sorcery_object_field_register_nodoc(sorcery, CONTACT_STATUS, "rtt",
		"0", OPT_UINT_T, 0, FLDSET(struct ast_sip_contact_status, rtt));

	return 0;
}

static void qualify_and_schedule_contact(struct ast_sip_contact *contact)
{
	int initial_interval;
	int max_time = ast_sip_get_max_initial_qualify_time();

	/* Delay initial qualification by a random fraction of the specified interval */
	if (max_time && max_time < contact->qualify_frequency) {
		initial_interval = max_time;
	} else {
		initial_interval = contact->qualify_frequency;
	}

	initial_interval = (int)((initial_interval * 1000) * ast_random_double());

	unschedule_qualify(contact);
	if (contact->qualify_frequency) {
		schedule_qualify(contact, initial_interval);
	} else {
		update_contact_status(contact, UNKNOWN, 0);
	}
}

static int qualify_and_schedule_cb_with_aor(void *obj, void *arg, int flags)
{
	struct ast_sip_contact *contact = obj;
	struct ast_sip_aor *aor = arg;

	contact->qualify_frequency = aor->qualify_frequency;
	contact->qualify_timeout = aor->qualify_timeout;
	contact->authenticate_qualify = aor->authenticate_qualify;

	qualify_and_schedule_contact(contact);

	return 0;
}

static int qualify_and_schedule_cb_without_aor(void *obj, void *arg, int flags)
{
	qualify_and_schedule_contact((struct ast_sip_contact *) obj);

	return 0;
}

/*!
 * \internal
 * \brief Qualify and schedule an aor's contacts
 *
 * \details For the given aor check if it has permanent contacts,
 *         qualify all contacts and schedule for checks if configured.
 */
static int qualify_and_schedule_all_cb(void *obj, void *arg, int flags)
{
	struct ast_sip_aor *aor = obj;
	struct ao2_container *contacts;

	contacts = ast_sip_location_retrieve_aor_contacts(aor);
	if (contacts) {
		ao2_callback(contacts, OBJ_NODATA, qualify_and_schedule_cb_with_aor, aor);
		ao2_ref(contacts, -1);
	}

	return 0;
}

/*!
 * \internal
 * \brief Unschedule all existing contacts
 */
static int unschedule_all_cb(void *obj, void *arg, int flags)
{
	struct sched_data *data = obj;

	AST_SCHED_DEL_UNREF(sched, data->id, ao2_ref(data, -1));

	return CMP_MATCH;
}

static void qualify_and_schedule_all(void)
{
	struct ast_variable *var = ast_variable_new("qualify_frequency >", "0", "");
	struct ao2_container *aors;
	struct ao2_container *contacts;

	if (!var) {
		return;
	}
	aors = ast_sorcery_retrieve_by_fields(ast_sip_get_sorcery(),
		"aor", AST_RETRIEVE_FLAG_MULTIPLE, var);

	ao2_callback(sched_qualifies, OBJ_NODATA | OBJ_MULTIPLE | OBJ_UNLINK, unschedule_all_cb, NULL);

	if (aors) {
		ao2_callback(aors, OBJ_NODATA, qualify_and_schedule_all_cb, NULL);
		ao2_ref(aors, -1);
	}

	contacts = ast_sorcery_retrieve_by_fields(ast_sip_get_sorcery(),
		"contact", AST_RETRIEVE_FLAG_MULTIPLE, var);
	if (contacts) {
		ao2_callback(contacts, OBJ_NODATA, qualify_and_schedule_cb_without_aor, NULL);
		ao2_ref(contacts, -1);
	}

	ast_variables_destroy(var);

}

int ast_sip_format_contact_ami(void *obj, void *arg, int flags)
{
	struct ast_sip_contact_wrapper *wrapper = obj;
	struct ast_sip_contact *contact = wrapper->contact;
	struct ast_sip_ami *ami = arg;
	struct ast_sip_contact_status *status;
	struct ast_str *buf;
	const struct ast_sip_endpoint *endpoint = ami->arg;

	buf = ast_sip_create_ami_event("ContactStatusDetail", ami);
	if (!buf) {
		return -1;
	}

	status = ast_sorcery_retrieve_by_id(
		ast_sip_get_sorcery(), CONTACT_STATUS,
		ast_sorcery_object_get_id(contact));

	ast_str_append(&buf, 0, "AOR: %s\r\n", wrapper->aor_id);
	ast_str_append(&buf, 0, "URI: %s\r\n", contact->uri);
	ast_str_append(&buf, 0, "UserAgent: %s\r\n", contact->user_agent);
	ast_str_append(&buf, 0, "RegExpire: %ld\r\n", contact->expiration_time.tv_sec);
	if (!ast_strlen_zero(contact->via_addr)) {
		ast_str_append(&buf, 0, "ViaAddress: %s", contact->via_addr);
		if (contact->via_port) {
			ast_str_append(&buf, 0, ":%d", contact->via_port);
		}
		ast_str_append(&buf, 0, "\r\n");
	}
	if (!ast_strlen_zero(contact->call_id)) {
		ast_str_append(&buf, 0, "CallID: %s\r\n", contact->call_id);
	}
	ast_str_append(&buf, 0, "Status: %s\r\n", ast_sip_get_contact_status_label(status->status));
	if (status->status == UNKNOWN) {
		ast_str_append(&buf, 0, "RoundtripUsec: N/A\r\n");
	} else {
		ast_str_append(&buf, 0, "RoundtripUsec: %" PRId64 "\r\n", status->rtt);
	}
	ast_str_append(&buf, 0, "EndpointName: %s\r\n",
			endpoint ? ast_sorcery_object_get_id(endpoint) : S_OR(contact->endpoint_name, ""));

	ast_str_append(&buf, 0, "ID: %s\r\n", ast_sorcery_object_get_id(contact));
	ast_str_append(&buf, 0, "AuthenticateQualify: %d\r\n", contact->authenticate_qualify);
	ast_str_append(&buf, 0, "OutboundProxy: %s\r\n", contact->outbound_proxy);
	ast_str_append(&buf, 0, "Path: %s\r\n", contact->path);
	ast_str_append(&buf, 0, "QualifyFrequency: %u\r\n", contact->qualify_frequency);
	ast_str_append(&buf, 0, "QualifyTimeout: %.3f\r\n", contact->qualify_timeout);

	astman_append(ami->s, "%s\r\n", ast_str_buffer(buf));
	ami->count++;
	
	ast_free(buf);
	ao2_cleanup(status);
	return 0;
}

static int format_contact_status_for_aor(void *obj, void *arg, int flags)
{
	struct ast_sip_aor *aor = obj;

	return ast_sip_for_each_contact(aor, ast_sip_format_contact_ami, arg);
}

static int format_ami_contact_status(const struct ast_sip_endpoint *endpoint,
		struct ast_sip_ami *ami)
{
	ami->arg = (void *)endpoint;
	return ast_sip_for_each_aor(endpoint->aors, format_contact_status_for_aor, ami);
}

static struct ast_sip_endpoint_formatter contact_status_formatter = {
	.format_ami = format_ami_contact_status
};

static void aor_observer_modified(const void *obj)
{
	struct ast_sip_aor *aor = (void *)obj;
	struct ao2_container *contacts;

	contacts = ast_sip_location_retrieve_aor_contacts(aor);
	if (contacts) {
		ao2_callback(contacts, OBJ_NODATA, qualify_and_schedule_cb_with_aor, aor);
		ao2_ref(contacts, -1);
	}
}

static int unschedule_contact_cb(void *obj, void *arg, int flags)
{
	unschedule_qualify(obj);

	return CMP_MATCH;
}

static void aor_observer_deleted(const void *obj)
{
	const struct ast_sip_aor *aor = obj;
	struct ao2_container *contacts;

	contacts = ast_sip_location_retrieve_aor_contacts(aor);
	if (contacts) {
		ao2_callback(contacts, OBJ_NODATA | OBJ_MULTIPLE, unschedule_contact_cb, NULL);
		ao2_ref(contacts, -1);
	}
}

static const struct ast_sorcery_observer observer_callbacks_options = {
	.created = aor_observer_modified,
	.updated = aor_observer_modified,
	.deleted = aor_observer_deleted
};

static int aor_update_endpoint_state(void *obj, void *arg, int flags)
{
	struct ast_sip_endpoint *endpoint = obj;
	const char *endpoint_name = ast_sorcery_object_get_id(endpoint);
	char *aor = arg;
	char *endpoint_aor;
	char *endpoint_aors;

	if (ast_strlen_zero(aor) || ast_strlen_zero(endpoint->aors)) {
		return 0;
	}

	endpoint_aors = ast_strdupa(endpoint->aors);
	while ((endpoint_aor = ast_strip(strsep(&endpoint_aors, ",")))) {
		if (!strcmp(aor, endpoint_aor)) {
			if (ast_sip_persistent_endpoint_update_state(endpoint_name, AST_ENDPOINT_ONLINE) == -1) {
				ast_log(LOG_WARNING, "Unable to find persistent endpoint '%s' for aor '%s'\n",
					endpoint_name, aor);
			}
		}
	}

	return 0;
}

static int on_aor_update_endpoint_state(void *obj, void *arg, int flags)
{
	struct ast_sip_aor *aor = obj;
	struct ao2_container *endpoints;
	RAII_VAR(struct ast_variable *, var, NULL, ast_variables_destroy);
	const char *aor_name = ast_sorcery_object_get_id(aor);
	char *aor_like;

	if (ast_strlen_zero(aor_name)) {
		return -1;
	}

	if (aor->permanent_contacts && ((int)(aor->qualify_frequency * 1000)) <= 0) {
		aor_like = ast_alloca(strlen(aor_name) + 3);
		sprintf(aor_like, "%%%s%%", aor_name);
		var = ast_variable_new("aors LIKE", aor_like, "");
		if (!var) {
			return -1;
		}
		endpoints = ast_sorcery_retrieve_by_fields(ast_sip_get_sorcery(),
			"endpoint", AST_RETRIEVE_FLAG_MULTIPLE, var);

		if (endpoints) {
		    /*
		     * Because aors are a string list, we have to use a pattern match but since a simple
		     * pattern match could return an endpoint that has an aor of "aaabccc" when searching
		     * for "abc", we still have to iterate over them to find an exact aor match.
		     */
		    ao2_callback(endpoints, 0, aor_update_endpoint_state, (char *)aor_name);
		    ao2_ref(endpoints, -1);
		}
	}

	return 0;
}

static int contact_update_endpoint_state(void *obj, void *arg, int flags)
{
	const struct ast_sip_contact *contact = obj;
	struct timeval tv = ast_tvnow();

	if (!ast_strlen_zero(contact->endpoint_name) && ((int)(contact->qualify_frequency * 1000)) <= 0 &&
		contact->expiration_time.tv_sec > tv.tv_sec) {

		if (ast_sip_persistent_endpoint_update_state(contact->endpoint_name, AST_ENDPOINT_ONLINE) == -1) {
			ast_log(LOG_WARNING, "Unable to find persistent endpoint '%s' for contact '%s/%s'\n",
				contact->endpoint_name, contact->aor, contact->uri);
			return -1;
		}
	}

	return 0;
}

static void update_all_unqualified_endpoints(void)
{
	struct ao2_container *aors;
	struct ao2_container *contacts;
	RAII_VAR(struct ast_variable *, var_aor, NULL, ast_variables_destroy);
	RAII_VAR(struct ast_variable *, var_contact, NULL, ast_variables_destroy);
	RAII_VAR(char *, time_now, NULL, ast_free);
	struct timeval tv = ast_tvnow();

	if (!(var_aor = ast_variable_new("contact !=", "", ""))) {
		return;
	}
	if (!(var_aor->next = ast_variable_new("qualify_frequency <=", "0", ""))) {
		return;
	}

	if (ast_asprintf(&time_now, "%ld", tv.tv_sec) == -1) {
		return;
	}
	if (!(var_contact = ast_variable_new("expiration_time >", time_now, ""))) {
		return;
	}
	if (!(var_contact->next = ast_variable_new("qualify_frequency <=", "0", ""))) {
		return;
	}

	aors = ast_sorcery_retrieve_by_fields(ast_sip_get_sorcery(),
		"aor", AST_RETRIEVE_FLAG_MULTIPLE, var_aor);
	if (aors) {
		ao2_callback(aors, OBJ_NODATA, on_aor_update_endpoint_state, NULL);
		ao2_ref(aors, -1);
	}

	contacts = ast_sorcery_retrieve_by_fields(ast_sip_get_sorcery(),
		"contact", AST_RETRIEVE_FLAG_MULTIPLE, var_contact);
	if (contacts) {
		ao2_callback(contacts, OBJ_NODATA, contact_update_endpoint_state, NULL);
		ao2_ref(contacts, -1);
	}
}

int ast_res_pjsip_init_options_handling(int reload)
{
	static const pj_str_t STR_OPTIONS = { "OPTIONS", 7 };

	if (reload) {
		qualify_and_schedule_all();
		return 0;
	}

	sched_qualifies = ao2_t_container_alloc(QUALIFIED_BUCKETS,
		sched_qualifies_hash_fn, sched_qualifies_cmp_fn,
		"Create container for scheduled qualifies");
	if (!sched_qualifies) {
		return -1;
	}

	if (pjsip_endpt_register_module(ast_sip_get_pjsip_endpoint(), &options_module) != PJ_SUCCESS) {
		ao2_cleanup(sched_qualifies);
		sched_qualifies = NULL;
		return -1;
	}

	if (pjsip_endpt_add_capability(ast_sip_get_pjsip_endpoint(), NULL, PJSIP_H_ALLOW,
		NULL, 1, &STR_OPTIONS) != PJ_SUCCESS) {
		pjsip_endpt_unregister_module(ast_sip_get_pjsip_endpoint(), &options_module);
		ao2_cleanup(sched_qualifies);
		sched_qualifies = NULL;
		return -1;
	}

	if (ast_sorcery_observer_add(ast_sip_get_sorcery(), "aor", &observer_callbacks_options)) {
		pjsip_endpt_unregister_module(ast_sip_get_pjsip_endpoint(), &options_module);
		ao2_cleanup(sched_qualifies);
		sched_qualifies = NULL;
		return -1;
	}

	internal_sip_register_endpoint_formatter(&contact_status_formatter);
	ast_manager_register_xml("PJSIPQualify", EVENT_FLAG_SYSTEM | EVENT_FLAG_REPORTING, ami_sip_qualify);
	ast_cli_register_multiple(cli_options, ARRAY_LEN(cli_options));

	update_all_unqualified_endpoints();
	qualify_and_schedule_all();

	return 0;
}

static int unschedule_for_aor_cb(void *obj, void *arg, int flags)
{
	struct sched_data *data = obj;
	struct ast_sip_aor *aor = arg;

	if (!strcmp(ast_sorcery_object_get_id(aor), data->contact->aor)) {
		AST_SCHED_DEL_UNREF(sched, data->id, ao2_ref(data, -1));
	}

	return 0;
}

void ast_res_pjsip_update_options(struct ast_sip_aor *aor)
{
	/* This can happen if an AOR is created and applied before OPTIONs code has been initialized */
	if (!sched_qualifies) {
		return;
	}

	ao2_callback(sched_qualifies, OBJ_NODATA | OBJ_UNLINK, unschedule_for_aor_cb, aor);
	qualify_and_schedule_all_cb(aor, NULL, 0);
}

void ast_res_pjsip_cleanup_options_handling(void)
{
	ast_cli_unregister_multiple(cli_options, ARRAY_LEN(cli_options));
	ast_manager_unregister("PJSIPQualify");
	internal_sip_unregister_endpoint_formatter(&contact_status_formatter);

	ast_sorcery_observer_remove(ast_sip_get_sorcery(), "aor", &observer_callbacks_options);
	pjsip_endpt_unregister_module(ast_sip_get_pjsip_endpoint(), &options_module);
	ao2_cleanup(sched_qualifies);
	sched_qualifies = NULL;
}
