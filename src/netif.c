/*
 * The process in the flask.
 *
 * Copyright (c) 2013, Alessandro Ghedini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>

#include <net/if.h>

#include <linux/rtnetlink.h>
#include <linux/veth.h>

#include "ut/utlist.h"

#include "netif.h"
#include "nl.h"
#include "printf.h"
#include "util.h"

struct netif {
    enum netif_type type;

    char *dev;
    char *name;

    struct netif *next, *prev;
} netif;

static void if_up(int sock, int if_index);
static void move_and_rename_if(int sock, pid_t pid, int i, char *new_name);
static void create_macvlan(int sock, int master, char *name);
static void create_ipvlan(int sock, int master, char *name);
static void create_veth_pair(int sock, char *name_out, char *name_in);

void netif_add(struct netif **ifs, enum netif_type type, char *dev, char *name) {
    struct netif *nif = malloc(sizeof(struct netif));
    fail_if(!nif, "OOM");

    nif->dev  = strdup(dev);
    nif->name = strdup(name);
    nif->type = type;

    DL_APPEND(*ifs, nif);
}

void netif_add_from_spec(struct netif **ifs, const char *spec) {
    _free_ char *tmp = NULL;
    _free_ char **opts = NULL;

    if (!spec) return;

    tmp = strdup(spec);
    fail_if(!tmp, "OOM");

    size_t c = split_str(tmp, &opts, ":");
    fail_if(!c, "Invalid netif spec '%s': not enough args", spec);

    if (if_nametoindex(opts[0])) {
        fail_if(c < 2, "Invalid netif spec '%s': not enough args",spec);

        netif_add(ifs, MOVE, opts[0], opts[1]);
    } else if (!strncmp(opts[0], "macvlan", 8)) {
        fail_if(c < 3, "Invalid netif spec '%s': not enough args",spec);

        netif_add(ifs, MACVLAN, opts[1], opts[2]);
    } else if (!strncmp(opts[0], "ipvlan", 8)) {
        fail_if(c < 3, "Invalid netif spec '%s': not enough args",spec);

        netif_add(ifs, IPVLAN, opts[1], opts[2]);
    } else if (!strncmp(opts[0], "veth", 5)) {
        fail_if(c < 3, "Invalid netif spec '%s': not enough args",spec);

        netif_add(ifs, VETH, opts[1], opts[2]);
    } else {
        fail_printf("Invalid netif spec '%s'", spec);
    }
}

void setup_netif(struct netif *ifs, pid_t pid) {
    int rc;
    _close_ int sock = nl_open();

    struct netif *i = NULL;

    DL_FOREACH(ifs, i) {
        unsigned int if_index = 0;

        switch (i->type) {
        case MACVLAN: {
            _free_ char *name = NULL;

            rc = asprintf(&name, "pflask-%d", pid);
            fail_if(rc < 0, "OOM");

            if_index = if_nametoindex(i->dev);
            sys_fail_if(!if_index, "Error searching for '%s'", i->dev);

            create_macvlan(sock, if_index, name);

            if_index = if_nametoindex(name);
            break;
        }

        case IPVLAN: {
            _free_ char *name = NULL;

            rc = asprintf(&name, "pflask-%d", pid);
            fail_if(rc < 0, "OOM");

            if_index = if_nametoindex(i->dev);
            sys_fail_if(!if_index, "Error searching for '%s'", i->dev);

            create_ipvlan(sock, if_index, name);

            if_index = if_nametoindex(name);
            break;
        }

        case VETH: {
            _free_ char *name = NULL;

            rc = asprintf(&name, "pflask-%d", pid);
            fail_if(rc < 0, "OOM");

            create_veth_pair(sock, i->dev, name);

            if_index = if_nametoindex(name);
            sys_fail_if(!if_index, "Error searching for '%s'", name);
            break;
        }

        case MOVE:
            if_index = if_nametoindex(i->dev);
            sys_fail_if(!if_index, "Error searching for '%s'", i->dev);
            break;
        }

        move_and_rename_if(sock, pid, if_index, i->name);
    }
}

void config_netif(void) {
    _close_ int sock = nl_open();
    if_up(sock, 1);
}

static void if_up(int sock, int if_index) {
    _free_ struct nlmsg *req = malloc(NLMSG_GOOD_SIZE);

    req->hdr.nlmsg_seq   = 1;
    req->hdr.nlmsg_type  = RTM_NEWLINK;
    req->hdr.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req->hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    req->msg.ifi.ifi_family  = AF_UNSPEC;
    req->msg.ifi.ifi_index   = if_index;
    req->msg.ifi.ifi_flags   = IFF_UP;
    req->msg.ifi.ifi_change  = IFF_UP;

    nl_send(sock, req);
    nl_recv(sock, req);

    if (req->hdr.nlmsg_type == NLMSG_ERROR)
        sys_fail_if(req->msg.err.error < 0,
                    "Error sending netlink request");
}

static void move_and_rename_if(int sock, pid_t pid, int if_index, char *new_name) {
    _free_ struct nlmsg *req = malloc(NLMSG_GOOD_SIZE);

    req->hdr.nlmsg_seq   = 1;
    req->hdr.nlmsg_type  = RTM_NEWLINK;
    req->hdr.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req->hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    req->msg.ifi.ifi_family  = AF_UNSPEC;
    req->msg.ifi.ifi_index   = if_index;

    rtattr_append(req, IFLA_NET_NS_PID, &pid, sizeof(pid));
    rtattr_append(req, IFLA_IFNAME, new_name, strlen(new_name) + 1);

    nl_send(sock, req);
    nl_recv(sock, req);

    if (req->hdr.nlmsg_type == NLMSG_ERROR)
        sys_fail_if(req->msg.err.error < 0,
                    "Error sending netlink request");
}

static void create_macvlan(int sock, int master, char *name) {
    struct rtattr *nested = NULL;

    _free_ struct nlmsg *req = malloc(NLMSG_GOOD_SIZE);

    req->hdr.nlmsg_seq   = 1;
    req->hdr.nlmsg_type  = RTM_NEWLINK;
    req->hdr.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req->hdr.nlmsg_flags = NLM_F_REQUEST |
                             NLM_F_CREATE  |
                             NLM_F_EXCL    |
                             NLM_F_ACK;

    req->msg.ifi.ifi_family  = AF_UNSPEC;

    nested = rtattr_start_nested(req, IFLA_LINKINFO);
    rtattr_append(req, IFLA_INFO_KIND, "macvlan", 8);
    rtattr_end_nested(req, nested);

    rtattr_append(req, IFLA_LINK, &master, sizeof(master));
    rtattr_append(req, IFLA_IFNAME, name, strlen(name) + 1);

    nl_send(sock, req);
    nl_recv(sock, req);

    if (req->hdr.nlmsg_type == NLMSG_ERROR)
        sys_fail_if(req->msg.err.error < 0,
                    "Error sending netlink request");
}

static void create_ipvlan(int sock, int master, char *name) {
    struct rtattr *nested = NULL;

    _free_ struct nlmsg *req = malloc(NLMSG_GOOD_SIZE);

    req->hdr.nlmsg_seq   = 1;
    req->hdr.nlmsg_type  = RTM_NEWLINK;
    req->hdr.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req->hdr.nlmsg_flags = NLM_F_REQUEST |
                             NLM_F_CREATE  |
                             NLM_F_EXCL    |
                             NLM_F_ACK;

    req->msg.ifi.ifi_family  = AF_UNSPEC;

    nested = rtattr_start_nested(req, IFLA_LINKINFO);
    rtattr_append(req, IFLA_INFO_KIND, "ipvlan", 7);
    rtattr_end_nested(req, nested);

    rtattr_append(req, IFLA_LINK, &master, sizeof(master));
    rtattr_append(req, IFLA_IFNAME, name, strlen(name) + 1);

    nl_send(sock, req);
    nl_recv(sock, req);

    if (req->hdr.nlmsg_type == NLMSG_ERROR)
        sys_fail_if(req->msg.err.error < 0,
                    "Error sending netlink request");
}

static void create_veth_pair(int sock, char *name_out, char *name_in) {
    struct rtattr *nested_info = NULL;
    struct rtattr *nested_data = NULL;
    struct rtattr *nested_peer = NULL;

    _free_ struct nlmsg *req = malloc(NLMSG_GOOD_SIZE);

    req->hdr.nlmsg_seq   = 1;
    req->hdr.nlmsg_type  = RTM_NEWLINK;
    req->hdr.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req->hdr.nlmsg_flags = NLM_F_REQUEST |
                             NLM_F_CREATE  |
                             NLM_F_EXCL    |
                             NLM_F_ACK;

    req->msg.ifi.ifi_family  = AF_UNSPEC;

    nested_info = rtattr_start_nested(req, IFLA_LINKINFO);
    rtattr_append(req, IFLA_INFO_KIND, "veth", 5);

    nested_data = rtattr_start_nested(req, IFLA_INFO_DATA);
    nested_peer = rtattr_start_nested(req, VETH_INFO_PEER);

    req->hdr.nlmsg_len += sizeof(struct ifinfomsg);
    rtattr_append(req, IFLA_IFNAME, name_in, strlen(name_in) + 1);

    rtattr_end_nested(req, nested_peer);
    rtattr_end_nested(req, nested_data);

    rtattr_end_nested(req, nested_info);

    rtattr_append(req, IFLA_IFNAME, name_out, strlen(name_out) + 1);

    nl_send(sock, req);
    nl_recv(sock, req);

    if (req->hdr.nlmsg_type == NLMSG_ERROR)
        sys_fail_if(req->msg.err.error < 0,
                    "Error sending netlink request");
}
