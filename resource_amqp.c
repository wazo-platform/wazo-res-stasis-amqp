/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2019 The Wazo Authors  (see the AUTHORS file)
 *
 * Nicolaos Ballas
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

/*! \file
 *
 * \brief /api-docs/amqp.{format} implementation- AMQP resources
 *
 * \author Nicolaos Ballas
 */

#include "asterisk.h"

#include "resource_amqp.h"
#include "asterisk/stasis_app.h"
#include "asterisk/stasis_amqp.h"

void ast_ari_amqp_stasis_subscribe(struct ast_variable *headers,
	struct ast_ari_amqp_stasis_subscribe_args *args,
	struct ast_ari_response *response)
{
	const char *app_name = args->application_name;
	const char *connection = args->connection;

	if (!app_name) {
		ast_ari_response_error(response, 400, "Invalid argument", "No application specified");
		return;
	}

	if (!connection) {
		ast_ari_response_error(response, 400, "Invalid argument", "No connection specified");
		return;
	}

	ast_log(LOG_ERROR, "TODO: ast_ari_amqp_stasis_subscribe\n");

	subscribe_to_stasis(app_name, connection);
	ast_ari_response_no_content(response);

}

void ast_ari_amqp_stasis_unsubscribe(struct ast_variable *headers,
	struct ast_ari_amqp_stasis_unsubscribe_args *args,
	struct ast_ari_response *response)
{
	ast_log(LOG_ERROR, "TODO: ast_ari_amqp_stasis_unsubscribe\n");
	ast_ari_response_no_content(response);
}