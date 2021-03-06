<?php

/**
 +-----------------------------------------------------------------------+
 | This file is part of the Roundcube Webmail client                     |
 |                                                                       |
 | Copyright (C) The Roundcube Dev Team                                  |
 | Copyright (C) Kolab Systems AG                                        |
 |                                                                       |
 | Licensed under the GNU General Public License version 3 or            |
 | any later version with exceptions for skins & plugins.                |
 | See the README file for a full license statement.                     |
 |                                                                       |
 | PURPOSE:                                                              |
 |   Perform a search on configured address books for the email          |
 |   address autocompletion                                              |
 +-----------------------------------------------------------------------+
 | Author: Thomas Bruederli <roundcube@gmail.com>                        |
 +-----------------------------------------------------------------------+
*/

if ($RCMAIL->action == 'group-expand') {
    $abook = $RCMAIL->get_address_book(rcube_utils::get_input_value('_source', rcube_utils::INPUT_GPC));
    if ($gid = rcube_utils::get_input_value('_gid', rcube_utils::INPUT_GET)) {
        $abook->set_group($gid);
        $abook->set_pagesize(9999);  // TODO: limit number of group members by config?

        $result  = $abook->list_records($RCMAIL->config->get('contactlist_fields'));
        $members = array();

        while ($result && ($record = $result->iterate())) {
            $emails = (array) $abook->get_col_values('email', $record, true);
            if (!empty($emails) && ($email = array_shift($emails))) {
                $members[] = format_email_recipient($email, rcube_addressbook::compose_list_name($record));
            }
        }

        $OUTPUT->command('replace_group_recipients', $gid, join(', ', array_unique($members)));
    }

    $OUTPUT->send();
}

$MAXNUM = (int) $RCMAIL->config->get('autocomplete_max', 15);
$mode   = (int) $RCMAIL->config->get('addressbook_search_mode');
$single = (bool) $RCMAIL->config->get('autocomplete_single');
$search = rcube_utils::get_input_value('_search', rcube_utils::INPUT_GPC, true);
$reqid  = rcube_utils::get_input_value('_reqid', rcube_utils::INPUT_GPC);

$contacts = array();

if (strlen($search) && ($book_types = rcmail_autocomplete_addressbooks())) {
    $sort_keys = array();
    $books_num = count($book_types);
    $search_lc = mb_strtolower($search);
    $mode     |= rcube_addressbook::SEARCH_GROUPS;

    foreach ($book_types as $abook_id) {
        $abook = $RCMAIL->get_address_book($abook_id);
        $abook->set_pagesize($MAXNUM);

        if ($result = $abook->search($RCMAIL->config->get('contactlist_fields'), $search, $mode, true, true, 'email')) {
            while ($record = $result->iterate()) {
                // Contact can have more than one e-mail address
                $email_arr = (array)$abook->get_col_values('email', $record, true);
                $email_cnt = count($email_arr);
                $idx       = 0;

                foreach ($email_arr as $email) {
                    if (empty($email)) {
                        continue;
                    }

                    $name    = rcube_addressbook::compose_list_name($record);
                    $contact = format_email_recipient($email, $name);

                    // skip entries that don't match
                    if ($email_cnt > 1 && strpos(mb_strtolower($contact), $search_lc) === false) {
                        continue;
                    }

                    $index = $contact;

                    // skip duplicates
                    if (empty($contacts[$index])) {
                        $contact = array(
                            'name'   => $contact,
                            'type'   => $record['_type'],
                            'id'     => $record['ID'],
                            'source' => $abook_id,
                        );

                        if (($display = rcube_addressbook::compose_search_name($record, $email, $name)) && $display != $contact['name']) {
                            $contact['display'] = $display;
                        }

                        // groups with defined email address will not be expanded to its members' addresses
                        if ($record['_type'] == 'group') {
                            $contact['email'] = $email;
                        }

                        $contacts[$index]  = $contact;
                        $sort_keys[$index] = sprintf('%s %03d', $contact['display'] ?: $name, $idx++);

                        if (count($contacts) >= $MAXNUM) {
                            break 2;
                        }
                    }

                    // skip redundant entries (show only first email address)
                    if ($single) {
                        break;
                    }
                }
            }
        }

        // also list matching contact groups
        if ($abook->groups && count($contacts) < $MAXNUM) {
            foreach ($abook->list_groups($search, $mode) as $group) {
                $abook->reset();
                $abook->set_group($group['ID']);

                $group_prop = $abook->get_group($group['ID']);

                // group (distribution list) with email address(es)
                if ($group_prop['email']) {
                    $idx = 0;
                    foreach ((array)$group_prop['email'] as $email) {
                        $index = format_email_recipient($email, $group['name']);

                        if (empty($contacts[$index])) {
                            $sort_keys[$index] = sprintf('%s %03d', $group['name'] , $idx++);
                            $contacts[$index]  = array(
                                'name'   => $index,
                                'email'  => $email,
                                'type'   => 'group',
                                'id'     => $group['ID'],
                                'source' => $abook_id,
                            );

                            if (count($contacts) >= $MAXNUM) {
                                break 3;
                            }
                        }
                    }
                }
                // show group with count
                else if (($result = $abook->count()) && $result->count) {
                    if (empty($contacts[$group['name']])) {
                        $sort_keys[$group['name']] = $group['name'];
                        $contacts[$group['name']]  = array(
                            'name'   => $group['name'] . ' (' . intval($result->count) . ')',
                            'type'   => 'group',
                            'id'     => $group['ID'],
                            'source' => $abook_id,
                        );

                        if (count($contacts) >= $MAXNUM) {
                            break 2;
                        }
                    }
                }
            }
        }
    }

    if (count($contacts)) {
        // sort contacts index
        asort($sort_keys, SORT_LOCALE_STRING);
        // re-sort contacts according to index
        foreach ($sort_keys as $idx => $val) {
            $sort_keys[$idx] = $contacts[$idx];
        }
        $contacts = array_values($sort_keys);
    }
}


// Allow autocomplete result optimization via plugin
$plugin = $RCMAIL->plugins->exec_hook('contacts_autocomplete_after', array(
    'search'   => $search,
    'contacts' => $contacts, // Provide already-found contacts to plugin if they are required
));

$contacts = $plugin['contacts'];

$OUTPUT->command('ksearch_query_results', $contacts, $search, $reqid);
$OUTPUT->send();


/**
 * Collect addressbook sources used for autocompletion
 */
function rcmail_autocomplete_addressbooks()
{
    global $RCMAIL;

    $source = rcube_utils::get_input_value('_source', rcube_utils::INPUT_GPC);

    if (strlen($source)) {
        $book_types = array($source);
    }
    else {
        $book_types = (array) $RCMAIL->config->get('autocomplete_addressbooks', 'sql');
    }

    $collected_recipients = $RCMAIL->config->get('collected_recipients');
    $collected_senders    = $RCMAIL->config->get('collected_senders');

    if (strlen($collected_recipients) && !in_array($collected_recipients, $book_types)) {
        $book_types[] = $collected_recipients;
    }

    if (strlen($collected_senders) && !in_array($collected_senders, $book_types)) {
        $book_types[] = $collected_senders;
    }

    return !empty($book_types) ? $book_types : null;
}
