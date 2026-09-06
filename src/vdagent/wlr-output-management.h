/* wlr-output-management.h - set monitor resolutions on wlroots-based
 * Wayland compositors (Hyprland, Sway, ...) via wlr-output-management-
 * unstable-v1. This is the wlroots-family counterpart to mutter.c's
 * ApplyMonitorsConfig -- see display.c's vdagent_display_set_monitor_config
 * for the dispatch between the two.

    Copyright 2024 Red Hat, Inc.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SRC_VDAGENT_WLR_OUTPUT_MANAGEMENT_H_
#define SRC_VDAGENT_WLR_OUTPUT_MANAGEMENT_H_

#include "udscs.h"
#include "display.h"

typedef struct VDAgentWlrOutputMgmt VDAgentWlrOutputMgmt;

/* connector_mapping: borrowed, refed -- same GHashTable (connector name ->
 * SPICE display id) that vdagent_mutter_create() takes, owned by
 * VDAgentDisplay. */
VDAgentWlrOutputMgmt *vdagent_wlr_output_mgmt_create(GHashTable *connector_mapping);
void vdagent_wlr_output_mgmt_destroy(VDAgentWlrOutputMgmt *mgmt);

/* Attempts to apply mon_config via wlr-output-management-unstable-v1.
 *
 * Returns FALSE if the compositor doesn't implement the protocol at all
 * (caller should fall back to another backend, e.g. Mutter's
 * ApplyMonitorsConfig) or if it's never even seen a head yet.
 *
 * Returns TRUE once a request has actually been sent to the compositor --
 * this does NOT mean the compositor accepted it. The outcome (succeeded/
 * failed/cancelled) arrives asynchronously and is reported back to
 * vdagentd via vdagent_display_send_daemon_guest_res() from within this
 * file once known, the same way vdagent_x11_set_monitor_config()'s RandR
 * changes eventually surface through an X11 screen-change event. */
gboolean vdagent_wlr_output_mgmt_set_config(VDAgentWlrOutputMgmt *mgmt, VDAgentDisplay *display,
                                             VDAgentMonitorsConfig *mon_config);

#endif /* SRC_VDAGENT_WLR_OUTPUT_MANAGEMENT_H_ */
