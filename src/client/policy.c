/*
 *	wicked client related policy functions
 *
 *	Copyright (C) 2010-2014 SUSE LINUX Products GmbH, Nuernberg, Germany.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, see <http://www.gnu.org/licenses/> or write
 *	to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *	Boston, MA 02110-1301 USA.
 *
 *	Authors:
 *		Pawel Wieczorkiewicz <pwieczorkiewicz@suse.de>
 *
 */

#include <unistd.h>
#include <sys/types.h>
#include <ctype.h>

#include <wicked/fsm.h>
#include <wicked/netinfo.h>
#include <wicked/objectmodel.h>
#include <wicked/dbus-errors.h>
#include <wicked/logging.h>
#include <wicked/xml.h>

#include "client/ifconfig.h"

/*
 * Given the configuration for a device, generate a UUID that uniquely
 * identifies this configuration. We want to use this later to check
 * whether the configuration changed.
 *
 * We do this by hashing the XML configuration using a reasonably
 * collision free SHA hash algorithm, and storing that in a UUIDv5.
 */
ni_bool_t
ni_ifconfig_generate_uuid(const xml_node_t *config, ni_uuid_t *uuid)
{
	/* UUIDv5 of https://github.com/openSUSE/wicked in the URL
	 * namespace as our private namespace for the config UUIDs:
	 *      c89756cc-b7fb-569b-b7f0-49a400fa41fe
	 */
	static const ni_uuid_t ns = {
		.octets = {
			0xc8, 0x97, 0x56, 0xcc, 0xb7, 0xfb, 0x56, 0x9b,
			0xb7, 0xf0, 0x49, 0xa4, 0x00, 0xfa, 0x41, 0xfe
		}
	};
	memset(uuid, 0, sizeof(*uuid));

	/* Generate a version 5 (SHA1) UUID, of the node _content_ */
	return xml_node_content_uuid(config, 5, &ns, uuid) == 0;
}

static xml_node_t *
__ni_policy_add_to_match(xml_node_t *policy, const char *name, const char *value)
{
	if (policy && !ni_string_empty(name)) {
		xml_node_t *match = xml_node_get_child(policy, NI_NANNY_IFPOLICY_MATCH);

		if (match)
			return xml_node_new_element(name, match, value);
	}

	return NULL;
}

ni_bool_t
ni_ifpolicy_match_add_min_state(xml_node_t *policy, ni_fsm_state_t state)
{
	if (ni_ifworker_is_valid_state(state)) {
		const char *sname = ni_ifworker_state_name(state);

		if (__ni_policy_add_to_match(policy,
		    NI_NANNY_IFPOLICY_MATCH_MIN_STATE, sname)) {
			return TRUE;
		}
	}

	return FALSE;
}

ni_bool_t
ni_ifpolicy_match_add_link_type(xml_node_t *policy, unsigned int type)
{
	const char *linktype;

	linktype = ni_linktype_type_to_name(type);
	if (__ni_policy_add_to_match(policy,
	    NI_NANNY_IFPOLICY_MATCH_LINK_TYPE, linktype)) {
		return TRUE;
	}

	return FALSE;
}

char *
ni_ifpolicy_name_from_ifname(const char *ifname)
{
	ni_stringbuf_t buf = NI_STRINGBUF_INIT_DYNAMIC;
	size_t len, i;

	/*
	 * The policy name is used in dbus path which allows to
	 * use only "[A-Z][a-z][0-9]_" elements separated by "/".
	 *
	 * Just use some simple encoding for valid netdev name
	 * characters and return new policy name or NULL.
	 */
	len = ni_string_len(ifname);
	for (i = 0; i < len; ++i) {
		if (i == 0) {
			ni_stringbuf_puts(&buf, "policy__");
		}
		if (isalnum((unsigned char)ifname[i])) {
			ni_stringbuf_putc(&buf, ifname[i]);
			continue;
		}
		switch (ifname[i]) {
			case '_':
				ni_stringbuf_putc(&buf, '_');
				ni_stringbuf_putc(&buf, '_');
				break;
			case '.':
				ni_stringbuf_putc(&buf, '_');
				ni_stringbuf_putc(&buf, 'd');
				break;
			case '-':
				ni_stringbuf_putc(&buf, '_');
				ni_stringbuf_putc(&buf, 'm');
				break;
			default:
				ni_stringbuf_destroy(&buf);
				return NULL;
		}
	}
	return buf.string;
}

ni_bool_t
ni_ifpolicy_name_is_valid(const char *name)
{
	size_t i, len;

	if (!(len = ni_string_len(name)))
		return FALSE;

	for (i = 0; i < len; ++i) {
		if(isalnum((unsigned char)name[i]) ||
				name[i] == '_')
			continue;
		return FALSE;
	}

	return TRUE;
}


/*
 * Generate a <match> node for ifpolicy
 */
xml_node_t *
ni_ifpolicy_generate_match(const ni_string_array_t *ifnames, const char *cond)
{
	xml_node_t *mnode = NULL;
	xml_node_t *cnode = NULL;

	if (!(mnode = xml_node_new(NI_NANNY_IFPOLICY_MATCH, NULL)))
		return NULL;

	/* Always true condition */
	if (!ifnames || 0 == ifnames->count) {
		if (!xml_node_new_element(NI_NANNY_IFPOLICY_MATCH_ALWAYS_TRUE, mnode, NULL))
			goto error;
	}
	else {
		unsigned int i;

		if (ni_string_empty(cond))
			cond = NI_NANNY_IFPOLICY_MATCH_COND_OR;

		if (!(cnode = xml_node_new(cond, mnode)))
			goto error;

		for (i = 0; i < ifnames->count; i++) {
			 const char *ifname = ifnames->data[i];

			if (!xml_node_new_element(NI_NANNY_IFPOLICY_MATCH_DEV, cnode, ifname))
				goto error;
		}
	}

	return mnode;

error:
	xml_node_free(mnode);
	xml_node_free(cnode);
	return NULL;
}


/*
 * Convert ifconfig to ifpolicy format
 */
xml_node_t *
ni_convert_cfg_into_policy_node(const xml_node_t *ifcfg, xml_node_t *match, const char *name, const char *origin)
{
	xml_node_t *ifpolicy;
	ni_uuid_t uuid;
	xml_node_t *node;

	if (!ifcfg || !match || ni_string_empty(name) || ni_string_empty(origin))
		return NULL;

	ifpolicy = xml_node_new(NI_NANNY_IFPOLICY, NULL);
	xml_node_reparent(ifpolicy, xml_node_clone_ref(match));

	xml_node_add_attr(ifpolicy, NI_NANNY_IFPOLICY_NAME, name);

	xml_node_add_attr(ifpolicy, NI_NANNY_IFPOLICY_ORIGIN, origin);
	ni_uuid_generate(&uuid);
	xml_node_add_attr(ifpolicy, NI_NANNY_IFPOLICY_UUID, ni_uuid_print(&uuid));

	/* clone <interface> into policy and rename to <merge> */
	node = xml_node_clone(ifcfg, ifpolicy);
	ni_string_dup(&node->name, NI_NANNY_IFPOLICY_MERGE);

	return ifpolicy;
}

xml_document_t *
ni_convert_cfg_into_policy_doc(xml_document_t *ifconfig)
{
	xml_node_t *root, *ifnode, *ifname, *match;
	const char *origin;

	if (xml_document_is_empty(ifconfig))
		return NULL;

	root = xml_document_root(ifconfig);
	origin = xml_node_location_filename(root);

	for (ifnode = root->children; ifnode; ifnode = ifnode->next) {
		if (ni_ifpolicy_is_valid(ifnode)) {
			const char *name = ni_ifpolicy_get_name(ifnode);
			ni_debug_ifconfig("Ignoring already existing %s named %s from %s",
				NI_NANNY_IFPOLICY, name, origin);
			continue;
		}
		else if (ni_ifconfig_is_policy(ifnode)) {
			ni_debug_ifconfig("Ignoring already existing, noname %s from %s",
				NI_NANNY_IFPOLICY, origin);
			continue;
		}

		if (!ni_ifconfig_is_config(ifnode)) {
			ni_error("Invalid object found in file %s: neither an %s nor %s",
				origin, NI_CLIENT_IFCONFIG, NI_NANNY_IFPOLICY);
			return NULL;
		}

		ifname = xml_node_get_child(ifnode, NI_CLIENT_IFCONFIG_MATCH_NAME);
		if (!ifname || ni_string_empty(ifname->cdata))
			return NULL;

		if (!(match = ni_ifpolicy_generate_match(NULL, NULL)))
			return NULL;

		xml_node_add_child(root,
			ni_convert_cfg_into_policy_node(ifnode, match, ifname->cdata, origin));
	}

	return ifconfig;
}
