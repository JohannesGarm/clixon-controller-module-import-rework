/*
 *
  ***** BEGIN LICENSE BLOCK *****

  Copyright (C) 2023 Olof Hagsand

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  ***** END LICENSE BLOCK *****
  *
  * These functions are called from CLISPECs, ie controller_operation.cli or /_configure.cli
  */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <unistd.h>
#include <fnmatch.h>
#include <signal.h> /* matching strings */
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/param.h>
#include <netinet/in.h>

/* clicon */
#include <cligen/cligen.h>
#include <clixon/clixon.h>
#include <clixon/clixon_cli.h>
#include <clixon/cli_generate.h>

/* Controller includes */
#include "controller.h"
#include "controller_lib.h"
#include "controller_cli_callbacks.h"

/*!
 *
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
cli_apipath(clixon_handle h,
            cvec         *cvv,
            char         *mtpoint,
            char         *api_path_fmt,
            int          *cvvi,
            char        **api_path)
{
    int        retval = -1;
    char      *api_path_fmt01 = NULL;
    yang_stmt *yspec0;

    if (mtpoint){
        if ((yspec0 = clicon_dbspec_yang(h)) == NULL){
            clixon_err(OE_FATAL, 0, "No DB_SPEC");
            goto done;
        }
        /* Get and combined api-path01 */
        if (mtpoint_paths(yspec0, mtpoint, api_path_fmt, &api_path_fmt01) < 0)
            goto done;
        if (api_path_fmt2api_path(api_path_fmt01, cvv, api_path, cvvi) < 0)
            goto done;
    }
    else{
        if (api_path_fmt2api_path(api_path_fmt, cvv, api_path, cvvi) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (api_path_fmt01)
        free(api_path_fmt01);
    return retval;
}

/*!
 *
 * @param[in]  h    Clixon handle
 * @retval     0    OK
 * @retval    -1    Error
 */
static int
cli_apipath2xpath(clixon_handle h,
                  cvec         *cvv,
                  char         *mtpoint,
                  char         *api_path_fmt,
                  char        **xpath,
                  cvec        **nsc)
{
    int        retval = -1;
    yang_stmt *yspec0;
    char      *api_path = NULL;
    int        cvvi = 0;

    if (cli_apipath(h, cvv, mtpoint, api_path_fmt, &cvvi, &api_path) < 0)
        goto done;
    if ((yspec0 = clicon_dbspec_yang(h)) == NULL){
        clixon_err(OE_FATAL, 0, "No DB_SPEC");
        goto done;
    }
    if (api_path2xpath(api_path, yspec0, xpath, nsc, NULL) < 0)
        goto done;
    if (*xpath == NULL){
        clixon_err(OE_FATAL, 0, "Invalid api-path: %s", api_path);
        goto done;
    }
    retval = 0;
 done:
    if (api_path)
        free(api_path);
    return retval;
}

/*! Send get yanglib of all mountpoints to backend and only return devices/yang-libs that match pattern
 *
 * @param[in]  h         Clixon handle
 * @param[in]  pattern   Name glob pattern
 * @param[in]  single    pattern is a single device that can be used in an xpath
 * @param[in]  yanglib   0: only device name, 1: Also include config/yang-librarylib
 * @param[out] xdevsp    XML on the form <devices><device><name>x</name>...
 * @retval     0         OK
 * @retval    -1         Error
 * XXX: see https://github.com/clicon/clixon/issues/485
 */
int
rpc_get_yanglib_mount_match(clixon_handle h,
                            char         *pattern,
                            int           single,
                            int           yanglib,
                            cxobj       **xdevsp)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *xtop = NULL;
    cxobj *xrpc;
    cxobj *xdevs = NULL;
    cxobj *xdev;
    char  *devname;
    cxobj *xret = NULL;
    cxobj *xerr;

    clixon_debug(1, "%s", __FUNCTION__);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<get>");
    cprintf(cb, "<filter type=\"xpath\"");
    cprintf(cb, " select=\"/ctrl:devices/ctrl:device");
    if (single)
        cprintf(cb, "[ctrl:name='%s']", pattern);
    if (yanglib){
        cprintf(cb, "/ctrl:config");
        // XXX: see https://github.com/clicon/clixon/issues/485
        //        cprintf(cb, "/yanglib:yang-library");
    }
    else
        cprintf(cb, "/ctrl:name");
    cprintf(cb, "\"");
    cprintf(cb, " xmlns:ctrl=\"%s\" xmlns:yanglib=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\">",
                    CONTROLLER_NAMESPACE);

    cprintf(cb, "</filter>");
    cprintf(cb, "</get>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xerr = xpath_first(xret, NULL, "rpc-reply/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if ((xdevs = xpath_first(xret, NULL, "rpc-reply/data/devices")) != NULL){
        xdev = NULL;
        while ((xdev = xml_child_each(xdevs, xdev, CX_ELMNT)) != NULL) {
            if ((devname = xml_find_body(xdev, "name")) == NULL ||
                fnmatch(pattern, devname, 0) == 0) /* Match */
                xml_flag_set(xdev, XML_FLAG_MARK);
        }
        /* 2. Remove all unmarked nodes, ie non-matching nodes */
        if (xml_tree_prune_flagged_sub(xdevs, XML_FLAG_MARK, 1, NULL) < 0)
            goto done;
        /* Double check that there is at least one device */
        if (xdevsp && xpath_first(xdevs, NULL, "device/name") != NULL){
            *xdevsp = xdevs;
            xml_rm(*xdevsp);
        }
    }
    retval = 0;
 done:
    if (xtop)
        xml_free(xtop);
    if (xret)
        xml_free(xret);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Specialization of clixon cli_show_auto to handle device globs
 *
 * @param[in]  h    Clixon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables
 * @param[in]  argv Vector of function arguments
 * @retval     0    OK
 * @retval    -1    Error
 * @see cli_show_auto  Original function for description, arguments etc
 * @see cli_dbxml_devs Similar controller handling
 */
int
cli_show_auto_devs(clixon_handle h,
                   cvec         *cvv,
                   cvec         *argv)
{
    int              retval = -1;
    char            *dbname;
    enum format_enum format = FORMAT_XML;
    cvec            *nsc = NULL;
    int              pretty = 1;
    char            *prepend = NULL;
    int              state = 0;
    char            *withdefault = NULL; /* RFC 6243 modes */
    char            *extdefault = NULL; /* with extended tagged modes */
    int              argc = 0;
    char            *xpath = NULL;
    char            *api_path_fmt;  /* xml key format */
    char            *str;
    char            *mtpoint = NULL;
    cg_var          *cv;
    char            *pattern;
    cxobj           *xdevs = NULL;
    cxobj           *xdev;
    char            *devname;
    int              devices = 0;
    cbuf            *api_path_fmt_cb = NULL;    /* xml key format */
    int              i;
    int              fromroot = 0;

    if (cvec_len(argv) < 2){
        clixon_err(OE_PLUGIN, EINVAL, "Received %d arguments. Expected:: <api-path-fmt>* <datastore> [<format> <pretty> <state> <default> <prepend>]", cvec_len(argv));
        goto done;
    }
    if ((api_path_fmt_cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Concatenate all argv strings to a single string
     * Variant of cvec_concat_cb() where api-path-fmt may be interleaved with mtpoint,
     * eg /api-path-fmt2 mtpoint /api-path-fmt1 /api-path-fmt0
     */
    for (i=cvec_len(argv)-1; i>=0; i--){
        cv = cvec_i(argv, i);
        if ((str = cv_string_get(cv)) == NULL)
            continue;

        if (str && strncmp(str, "mtpoint:", strlen("mtpoint:")) == 0){
            mtpoint = str + strlen("mtpoint:");
            devices = strstr(mtpoint, "/ctrl:devices") != NULL;
            argc++;
            continue;
        }
        if (str[0] != '/')
            continue;
        argc++;
        cprintf(api_path_fmt_cb, "%s", str);
    }
    api_path_fmt = cbuf_get(api_path_fmt_cb);
    if (mtpoint == NULL)
        devices = strstr(api_path_fmt, "/clixon-controller:devices") != NULL;
    if (cvec_len(argv) <= argc){
        clixon_err(OE_PLUGIN, EINVAL, "Missing: <datastore>");
        goto done;
    }
    dbname = cv_string_get(cvec_i(argv, argc++));
    if (cvec_len(argv) > argc)
        if (cli_show_option_format(argv, argc++, &format) < 0)
            goto done;
    if (cvec_len(argv) > argc){
        if (cli_show_option_bool(argv, argc++, &pretty) < 0)
            goto done;
    }
    if (cvec_len(argv) > argc){
        if (cli_show_option_bool(argv, argc++, &state) < 0)
            goto done;
    }
    if (cvec_len(argv) > argc){
        if (cli_show_option_withdefault(argv, argc++, &withdefault, &extdefault) < 0)
            goto done;
    }
    if (cvec_len(argv) > argc){
        prepend = cv_string_get(cvec_i(argv, argc++));
    }
    if (cvec_len(argv) > argc){
        if (cli_show_option_bool(argv, argc++, &fromroot) < 0)
            goto done;
    }
    /* ad-hoc if devices device <name> is selected */
    if (devices && (cv = cvec_find(cvv, "name")) != NULL){
        pattern = cv_string_get(cv);
        if (rpc_get_yanglib_mount_match(h, pattern, 0, 0, &xdevs) < 0)
            goto done;
        if (xdevs == NULL){
            if (cli_apipath2xpath(h, cvv, mtpoint, api_path_fmt, &xpath, &nsc) < 0)
                goto done;
            if (cli_show_common(h, dbname, format, pretty, state,
                                withdefault, extdefault,
                                prepend, xpath, fromroot, nsc, 0) < 0)
                goto done;
        }
        else {
            xdev = NULL;
            while ((xdev = xml_child_each(xdevs, xdev, CX_ELMNT)) != NULL) {
                if ((devname = xml_find_body(xdev, "name")) == NULL)
                    continue;
                cv_string_set(cv, devname); /* replace name */
                /* aggregate to composite xpath */
                if (cli_apipath2xpath(h, cvv, mtpoint, api_path_fmt, &xpath, &nsc) < 0)
                    goto done;
                /* Meta-info /comment need to follow language, but only for XML here */
                if (format == FORMAT_XML)
                    cligen_output(stdout, "<!-- %s: -->\n", devname);
                else
                    cligen_output(stdout, "%s:", devname);
                if (cli_show_common(h, dbname, format, pretty, state,
                                    withdefault, extdefault,
                                    prepend, xpath, fromroot, nsc, 0) < 0)
                    goto done;
                if (xpath){
                    free(xpath);
                    xpath = NULL;
                }
                if (nsc){
                    free(nsc);
                    nsc = NULL;
                }
            }
        }
    }
    else {
        if (cli_apipath2xpath(h, cvv, mtpoint, api_path_fmt, &xpath, &nsc) < 0)
            goto done;
        if (cli_show_common(h, dbname, format, pretty, state,
                            withdefault, extdefault,
                            prepend, xpath, fromroot, nsc, 0) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (api_path_fmt_cb)
        cbuf_free(api_path_fmt_cb);
    if (xdevs)
        xml_free(xdevs);
    if (nsc)
        xml_nsctx_free(nsc);
    if (xpath)
        free(xpath);
    return retval;
}

/*! Common transaction notification handling from both async and poll
 *
 * @param[in]   h       CLixon handle
 * @param[in]   s       Notification socket
 * @param[in]   tidstr0 Transaction id string
 * @param[out]  match   Transaction id match
 * @param[out]  result  Transaction result
 * @param[out]  eof     EOF, socket closed
 * @retval      0       OK
 * @retval     -1       Fatal error
 */
static int
transaction_notification_handler(clixon_handle       h,
                                 int                 s,
                                 char               *tidstr0,
                                 int                *match,
                                 transaction_result *resultp,
                                 int                *eof)
{
    int                retval = -1;
    struct clicon_msg *reply = NULL;
    cxobj             *xt = NULL;
    cxobj             *xn;
    int                ret;
    char              *tidstr;
    char              *reason = NULL;
    char              *resstr;
    transaction_result result;
    void              *wh = NULL;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s tid:%s", __FUNCTION__, tidstr0);
    /* Need to set "intr" to enable ^C */
    if (clixon_resource_check(h, &wh, tidstr0, __FUNCTION__) < 0)
        goto done;
    if (clicon_msg_rcv(s, NULL, 1, &reply, eof) < 0){
        if (clixon_resource_check(h, &wh, tidstr0, __FUNCTION__) < 0)
            goto done;
        goto done;
    }
    if (clixon_resource_check(h, &wh, tidstr0, __FUNCTION__) < 0)
        goto done;
    if (*eof){
        clixon_err(OE_PROTO, ESHUTDOWN, "Socket unexpected close");
        close(s);
        goto done; /* Fatal, or possibly cli may reconnect */
    }
    if ((ret = clicon_msg_decode(reply, NULL, NULL, &xt, NULL)) < 0)
        goto done;
    if (ret == 0){ /* will not happen since no yspec ^*/
        clixon_err(OE_NETCONF, EFAULT, "Notification malformed");
        goto done;
    }
    clixon_debug_xml(1, xt, "Transaction");
    if ((xn = xpath_first(xt, 0, "notification/controller-transaction")) == NULL){
        clixon_err(OE_NETCONF, EFAULT, "Notification malformed");
        goto done;
    }
    reason = xml_find_body(xn, "reason");
    if ((tidstr = xml_find_body(xn, "tid")) == NULL){
        clixon_err(OE_NETCONF, EFAULT, "Notification malformed: no tid");
        goto done;
    }
    if (tidstr0 && strcmp(tidstr0, tidstr) == 0 && match)
        *match = 1;
    if ((resstr = xml_find_body(xn, "result")) == NULL){
        clixon_err(OE_NETCONF, EFAULT, "Notification malformed: no result");
        goto done;
    }
    if ((result = transaction_result_str2int(resstr)) != TR_SUCCESS){
        clixon_log(h, LOG_NOTICE, "%s: pid: %u Transaction %s failed: %s",
                   __FUNCTION__, getpid(), tidstr, reason?reason:"no reason");
        goto ok; // error == ^C
    }
    if (result)
        *resultp = result;
 ok:
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_DEFAULT, "%s %d", __FUNCTION__, retval);
    if (reply)
        free(reply);
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Send transaction error to backend
 *
 * @param[in] h      Clixon handle
 * @param[in] tidstr Transaction id
 * @retval    0      OK
 * @retval   -1      Error
 */
static int
send_transaction_error(clixon_handle h,
                       char         *tidstr)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *xtop = NULL;
    cxobj *xrpc;
    cxobj *xret = NULL;
    cxobj *xreply;
    cxobj *xerr;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<transaction-error xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<tid>%s</tid>", tidstr);
    cprintf(cb, "<origin>CLI</origin>");
    cprintf(cb, "<reason>Aborted by user</reason>");
    cprintf(cb, "</transaction-error>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    retval = 0;
 done:
    if (xtop)
        xml_free(xtop);
    if (xret)
        xml_free(xret);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Poll controller notification socket
 *
 * param[in]  h      Clixon handle
 * param[in]  tidstr Transaction identifier
 * param[out] result
 * @retval    0      OK
 * @retval   -1      Error
 * @see transaction_notification_cb
 */
static int
transaction_notification_poll(clixon_handle       h,
                              char               *tidstr,
                              transaction_result *result)
{
    int                retval = -1;
    int                eof = 0;
    int                s;
    int                match = 0;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s tid:%s", __FUNCTION__, tidstr);
    if ((s = clicon_data_int_get(h, "controller-transaction-notify-socket")) < 0){
        clixon_err(OE_EVENTS, 0, "controller-transaction-notify-socket is closed");
        goto done;
    }
    while (!match){
        if (transaction_notification_handler(h, s, tidstr, &match, result, &eof) < 0){
            if (eof)
                goto done;
            /* Interpret as user stop transaction: abort transaction */
            if (send_transaction_error(h, tidstr) < 0)
                goto done;
            cligen_output(stderr, "Aborted by user\n");
            break;
        }
    }
    if (match){
        switch (*result){
        case TR_ERROR:
            cligen_output(stderr, "Error\n"); // XXX: Not recoverable??
            break;
        case TR_FAILED:
            cligen_output(stderr, "Failed\n");
            break;
        case TR_INIT:
        case TR_SUCCESS:
            break;
        }
    }
    retval = 0;
 done:
    clixon_debug(CLIXON_DBG_DEFAULT, "%s %d", __FUNCTION__, retval);
    return retval;
}

/*! Read(pull) the config of one or several devices.
 *
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv : replace/merge
 * @retval    0      OK
 * @retval   -1      Error
 */
int
cli_rpc_pull(clixon_handle h,
             cvec         *cvv,
             cvec         *argv)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    cg_var    *cv;
    cxobj     *xtop = NULL;
    cxobj     *xrpc;
    cxobj     *xret = NULL;
    cxobj     *xreply;
    cxobj     *xerr;
    char      *op;
    char      *name = "*";
    cxobj     *xid;
    char      *tidstr;
    uint64_t   tid = 0;
    transaction_result result;

    if (argv == NULL || cvec_len(argv) != 1){
        clixon_err(OE_PLUGIN, EINVAL, "requires argument: replace/merge");
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <push>");
        goto done;
    }
    op = cv_string_get(cv);
    if (strcmp(op, "replace") != 0 && strcmp(op, "merge") != 0){
        clixon_err(OE_PLUGIN, EINVAL, "pull <type> argument is %s, expected \"validate\" or \"commit\"", op);
        goto done;
    }
    if ((cv = cvec_find(cvv, "name")) != NULL)
        name = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<config-pull xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    if (strcmp(op, "merge") == 0)
        cprintf(cb, "<merge>true</merge>");
    cprintf(cb, "</config-pull>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if ((xid = xpath_first(xreply, NULL, "tid")) == NULL){
        clixon_err(OE_CFG, 0, "No returned id");
        goto done;
    }
    tidstr = xml_body(xid);
    if (tidstr && parse_uint64(tidstr, &tid, NULL) <= 0)
        goto done;
    if (transaction_notification_poll(h, tidstr, &result) < 0)
        goto done;
    if (result == TR_SUCCESS)
        cligen_output(stderr, "OK\n");
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    return retval;
}

static int
cli_rpc_commit_diff_one(clixon_handle h,
                        char         *name)
{
    int     retval = -1;
    cbuf   *cb = NULL;
    cxobj  *xtop = NULL;
    cxobj  *xrpc;
    cxobj  *xreply;
    cxobj  *xerr;
    cxobj  *xdiff;
    cxobj  *xret = NULL;
    cxobj **vec = NULL;
    size_t  veclen;
    int     i;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<datastore-diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    cprintf(cb, "<config-type1>RUNNING</config-type1>");
    cprintf(cb, "<config-type2>ACTIONS</config-type2>");
    cprintf(cb, "</datastore-diff>");
    cprintf(cb, "</rpc>");
    if (xtop){
        xml_free(xtop);
        xtop = NULL;
    }
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if (xpath_vec(xreply, NULL, "diff", &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        if ((xdiff = vec[i]) != NULL &&
            xml_body(xdiff) != NULL)
            cligen_output(stdout, "%s", xml_body(xdiff));
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Make a controller commit diff variant
 *
 * @param[in] h    Clixon handle
 * @retval    0    OK
 * @retval   -1    Error
 */
static int
cli_rpc_commit_diff(clixon_handle h)
{
    int     retval = -1;
    cxobj  *xdevs = NULL;
    cxobj  *xdev;
    cvec   *nsc = NULL;
    cxobj **vec = NULL;
    size_t  veclen;
    char   *name;
    int     i;

    /* get all devices */
    if ((nsc = xml_nsctx_init("co", CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (clicon_rpc_get_config(h, NULL, "running", "co:devices/co:device/co:name", nsc,
                              "explicit", &xdevs) < 0)
        goto done;
    if (xpath_vec(xdevs, nsc, "devices/device/name", &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        xdev = vec[i];
        if ((name = xml_body(xdev)) != NULL){
            if (cli_rpc_commit_diff_one(h, name) < 0)
                goto done;
        }
    }
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    if (vec)
        free(vec);
    if (xdevs)
        xml_free(xdevs);
    return retval;
}

/*! Make a controller commit rpc with its many variants
 *
 * Relies on hardcoded "name" and "instance" variables in cvv
 * @param[in] h     Clixon handle
 * @param[in] cvv   Name pattern
 * @param[in] argv  Source:running/candidate,
                    actions:NONE/CHANGE/FORCE,
                    push:NONE/VALIDATE/COMMIT,
 * @retval    0     OK
 * @retval   -1     Error
 * @see controller-commit in clixon-controller.yang
 */
int
cli_rpc_controller_commit(clixon_handle h,
                          cvec         *cvv,
                          cvec         *argv)
{
    int                retval = -1;
    int                argc = 0;
    cbuf              *cb = NULL;
    cg_var            *cv;
    cxobj             *xtop = NULL;
    cxobj             *xrpc;
    cxobj             *xret = NULL;
    cxobj             *xreply;
    cxobj             *xerr;
    char              *push_type;
    char              *name = "*";
    cxobj             *xid;
    char              *tidstr;
    uint64_t           tid = 0;
    char              *actions_type;
    char              *source;
    transaction_result result;
    char              *service = NULL;

    if (argv == NULL || cvec_len(argv) != 3){
        clixon_err(OE_PLUGIN, EINVAL, "requires arguments: <datastore> <actions-type> <push-type>");
        goto done;
    }
    if ((cv = cvec_i(argv, argc++)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <datastore>");
        goto done;
    }
    source = cv_string_get(cv);
    if ((cv = cvec_i(argv, argc++)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <actions-type>");
        goto done;
    }
    actions_type = cv_string_get(cv);
    if (actions_type_str2int(actions_type) == -1){
        clixon_err(OE_PLUGIN, EINVAL, "<actions-type> argument is %s, expected NONE/CHANGE/FORCE", actions_type);
        goto done;
    }
    if ((cv = cvec_i(argv, argc++)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <push-type>");
        goto done;
    }
    push_type = cv_string_get(cv);
    if (push_type_str2int(push_type) == -1){
        clixon_err(OE_PLUGIN, EINVAL, "<push-type> argument is %s, expected NONE/VALIDATE/COMMIT", push_type);
        goto done;
    }
    if ((cv = cvec_find(cvv, "name")) != NULL)
        name = cv_string_get(cv);
    if ((cv = cvec_find(cvv, "instance")) != NULL){
        service = cv_string_get(cv);
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<controller-commit xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<device>%s</device>", name);
    cprintf(cb, "<push>%s</push>", push_type);
    cprintf(cb, "<actions>%s</actions>", actions_type);
    if (service && actions_type_str2int(actions_type) == AT_FORCE){
        cprintf(cb, "<service-instance>");
        if (xml_chardata_cbuf_append(cb, service) < 0)
            goto done;
        cprintf(cb, "</service-instance>");
    }
    cprintf(cb, "<source>ds:%s</source>", source); /* Note add datastore prefix */
    cprintf(cb, "</controller-commit>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if ((xid = xpath_first(xreply, NULL, "tid")) == NULL){
        clixon_err(OE_CFG, 0, "No returned id");
        goto done;
    }
    tidstr = xml_body(xid);
    if (tidstr && parse_uint64(tidstr, &tid, NULL) <= 0)
        goto done;
    if (transaction_notification_poll(h, tidstr, &result) < 0)
        goto done;
    if (result != TR_SUCCESS)
        goto ok;
    /* Interpret actions and no push as diff */
    if (actions_type_str2int(actions_type) != AT_NONE &&
        push_type_str2int(push_type) == PT_NONE){
        if (cli_rpc_commit_diff(h) < 0)
            goto done;
    }
    cligen_output(stderr, "OK\n");
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    return retval;
}

/*! Read the config of one or several devices
 *
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv : 0: close, 1: open, 2: reconnect
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_connection_change(clixon_handle h,
                      cvec         *cvv,
                      cvec         *argv)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    cg_var    *cv;
    cxobj     *xtop = NULL;
    cxobj     *xrpc;
    cxobj     *xret = NULL;
    cxobj     *xreply;
    cxobj     *xerr;
    char      *name = "*";
    char      *op;

    if (argv == NULL || cvec_len(argv) != 1){
        clixon_err(OE_PLUGIN, EINVAL, "requires argument: <operation>");
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <push>");
        goto done;
    }
    op = cv_string_get(cv);
    if ((cv = cvec_find(cvv, "name")) != NULL)
        name = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<connection-change xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    cprintf(cb, "<operation>%s</operation>", op);
    cprintf(cb, "</connection-change>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    return retval;
}

/*! Show controller device states
 *
 * @param[in]  h    Clixon handle
 * @param[in] cvv  : name pattern
 * @param[in] argv : "detail"?
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_show_devices(clixon_handle h,
                 cvec         *cvv,
                 cvec         *argv)
{
    int     retval = -1;
    cvec   *nsc = NULL;
    cxobj  *xc;
    cxobj  *xerr;
    cbuf   *cb = NULL;
    cxobj  *xn = NULL; /* XML of senders */
    char   *name;
    char   *state;
    char   *timestamp;
    char   *logmsg;
    char   *pattern = NULL;
    cg_var *cv;
    int     detail = 0;
    int     width;
    int     logw;
    int     i;
    char   *logstr = NULL;
    char   *p;

    if (argv != NULL && cvec_len(argv) != 1){
        clixon_err(OE_PLUGIN, EINVAL, "optional argument: <detail>");
        goto done;
    }
    if (cvec_len(argv) == 1){
        if ((cv = cvec_i(argv, 0)) == NULL){
            clixon_err(OE_PLUGIN, 0, "Error when accessing argument <detail>");
            goto done;
        }
        detail = strcmp(cv_string_get(cv), "detail")==0;
    }
    if ((cv = cvec_find(cvv, "name")) != NULL)
        pattern = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    /* Get config */
    if ((nsc = xml_nsctx_init("co", CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (detail){
        if (clicon_rpc_get(h, "co:devices", nsc, CONTENT_ALL, -1, "report-all", &xn) < 0)
            goto done;
    }
    else{
        /* Avoid including moint-point which triggers a lot of extra traffic */
        if (clicon_rpc_get(h,
                           "co:devices/co:device/co:name | co:devices/co:device/co:conn-state | co:devices/co:device/co:conn-state-timestamp | co:devices/co:device/co:logmsg",
                           nsc, CONTENT_ALL, -1, "explicit", &xn) < 0)
            goto done;
    }
    if ((xerr = xpath_first(xn, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get devices");
        goto done;
    }
    /* Change top from "data" to "devices" */
    if ((xc = xml_find_type(xn, NULL, "devices", CX_ELMNT)) != NULL){
        if (xml_rootchild_node(xn, xc) < 0)
            goto done;
        xn = xc;
        if (detail){
            xc = NULL;
            while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL) {
                if (strcmp(xml_name(xc), "device") != 0)
                    continue;
                name = xml_find_body(xc, "name");
                if (pattern != NULL && fnmatch(pattern, name, 0) != 0)
                    continue;
                if (clixon_xml2file(stdout, xc, 0, 1, NULL, cligen_output, 0, 1) < 0)
                    goto done;
            }
        }
        else {
            width = cligen_terminal_width(cli_cligen(h));
            logw = width - 58;
            cligen_output(stdout, "%-23s %-10s %-22s %-*s\n", "Name", "State", "Time", width-58, "Logmsg");
            for (i=0; i<width; i++)
                cligen_output(stdout, "=");
            cligen_output(stdout, "\n");
            xc = NULL;
            while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL) {
                if ((logstr = calloc(logw+1, sizeof(char))) == NULL){
                    clixon_err(OE_UNIX, errno, "calloc");
                    goto done;
                }
                if (strcmp(xml_name(xc), "device") != 0)
                    continue;
                name = xml_find_body(xc, "name");
                if (pattern != NULL && fnmatch(pattern, name, 0) != 0)
                    continue;
                cligen_output(stdout, "%-24s",  name);
                state = xml_find_body(xc, "conn-state");
                cligen_output(stdout, "%-11s",  state?state:"");
                if ((timestamp = xml_find_body(xc, "conn-state-timestamp")) != NULL){
                    /* Remove 6 us digits */
                    if ((p = rindex(timestamp, '.')) != NULL)
                        *p = '\0';
                }
                cligen_output(stdout, "%-23s", timestamp?timestamp:"");
                if ((logmsg = xml_find_body(xc, "logmsg")) != NULL){
                    strncpy(logstr, logmsg, logw);
                    logstr[logw] = '\0';
                    cligen_output(stdout, "%s",  logstr);
                }
                cligen_output(stdout, "\n");
            }
        }
    }
    retval = 0;
 done:
    if (logstr)
        free(logstr);
    if (nsc)
        cvec_free(nsc);
    if (xn)
        xml_free(xn);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Show controller device states
 *
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv : "detail"?
 * @retval    0    OK
 * @retval   -1    Error
 * @see cli_process_control
 */
int
cli_show_services_process(clixon_handle h,
                          cvec         *cvv,
                          cvec         *argv)
{
    int            retval = -1;
    char          *name;
    char          *opstr;
    cbuf          *cb = NULL;
    cxobj         *xret = NULL;
    cxobj         *xerr;
    cxobj         *x;
    char          *active = "false";
    char          *status = "unknown";

    name = "Action process";
    opstr = "status";
    if (clixon_process_op_str2int(opstr) == -1){
        clixon_err(OE_UNIX, 0, "No such process op: %s", opstr);
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, " %s", NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, ">");
    cprintf(cb, "<process-control xmlns=\"%s\">", CLIXON_LIB_NS);
    cprintf(cb, "<name>%s</name>", name);
    cprintf(cb, "<operation>%s</operation>", opstr);
    cprintf(cb, "</process-control>");
    cprintf(cb, "</rpc>");
    if (clicon_rpc_netconf(h, cbuf_get(cb), &xret, NULL) < 0)
        goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if ((x = xpath_first(xret, 0, "rpc-reply/active")) != NULL){
        active = xml_body(x);
    }
    if ((x = xpath_first(xret, 0, "rpc-reply/status")) != NULL){
        status = xml_body(x);
    }
    cligen_output(stdout, "Services status: %s, active: %s\n", status, active);
    retval = 0;
 done:
    if (xret)
        xml_free(xret);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Show controller device states
 *
 * @param[in] h
 * @param[in] cvv
 * @param[in] argv : "last" or "all"
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_show_transactions(clixon_handle h,
                      cvec         *cvv,
                      cvec         *argv)
{
    int                retval = -1;
    cvec              *nsc = NULL;
    cxobj             *xc;
    cxobj             *xerr;
    cbuf              *cb = NULL;
    cxobj             *xn = NULL; /* XML of senders */
    cg_var            *cv;
    int                all = 0;

    if (argv == NULL || cvec_len(argv) != 1){
        clixon_err(OE_PLUGIN, EINVAL, "requires argument: <operation>");
        goto done;
    }
    if ((cv = cvec_i(argv, 0)) == NULL){
        clixon_err(OE_PLUGIN, 0, "Error when accessing argument <all>");
        goto done;
    }
    all = strcmp(cv_string_get(cv), "all")==0;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    /* Get config */
    if ((nsc = xml_nsctx_init("co", CONTROLLER_NAMESPACE)) == NULL)
        goto done;
    if (clicon_rpc_get(h, "co:transactions", nsc, CONTENT_ALL, -1, "report-all", &xn) < 0)
        goto done;
    if ((xerr = xpath_first(xn, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get transactions");
        goto done;
    }
    /* Change top from "data" to "devices" */
    if ((xc = xml_find_type(xn, NULL, "transactions", CX_ELMNT)) != NULL){
        if (xml_rootchild_node(xn, xc) < 0)
            goto done;
        xn = xc;
        if (all){
            xn = xc;
            xc = NULL;
            while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL) {
                if (clixon_xml2file(stdout, xc, 0, 1, NULL, cligen_output, 0, 1) < 0)
                    goto done;
            }
        }
        else{
            if ((xc = xml_child_i(xn, xml_child_nr(xn) - 1)) != NULL){
                if (clixon_xml2file(stdout, xc, 0, 1, NULL, cligen_output, 0, 1) < 0)
                    goto done;
            }
        }
    }
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    if (xn)
        xml_free(xn);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Show controller client sessions
 *
 * @param[in] h
 * @param[in] cvv
 * @param[in] argv : "detail"?
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_show_sessions(clixon_handle h,
                  cvec         *cvv,
                  cvec         *argv)
{
    int      retval = -1;
    cvec    *nsc = NULL;
    cxobj   *xret = NULL;
    cxobj   *xerr;
    cbuf    *cb = NULL;
    cxobj   *xsess;
    cxobj  **vec = NULL;
    size_t   veclen;
    char    *b;
    int      i;
    cg_var  *cv;
    int      detail = 0;
    uint32_t session_id;

    if (argv != NULL && cvec_len(argv) != 1){
        clixon_err(OE_PLUGIN, EINVAL, "optional argument: <detail>");
        goto done;
    }
    if (cvec_len(argv) == 1){
        if ((cv = cvec_i(argv, 0)) == NULL){
            clixon_err(OE_PLUGIN, 0, "Error when accessing argument <detail>");
            goto done;
        }
        detail = strcmp(cv_string_get(cv), "detail")==0;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    /* Get config */
    if ((nsc = xml_nsctx_init("ncm", "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring")) == NULL)
        goto done;
    if (clicon_rpc_get(h, "ncm:netconf-state/ncm:sessions", nsc, CONTENT_NONCONFIG, -1, "report-all", &xret) < 0)
        goto done;
    if ((xerr = xpath_first(xret, NULL, "/rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get devices");
        goto done;
    }
    if (xpath_vec(xret, NULL, "netconf-state/sessions/session", &vec, &veclen) < 0)
        goto done;
    if (detail && veclen){
        cligen_output(stdout, "%-8s %-10s %-15s %-15s\n", "Id", "User", "Type", "Time");
        cligen_output(stdout, "===============================================================\n");
    }
    clicon_session_id_get(h, &session_id);
    for (i=0; i<veclen; i++){
        xsess = vec[i];
        if (detail){
            if (clixon_xml2file(stdout, xsess, 0, 1, NULL, cligen_output, 0, 1) < 0)
                goto done;
        }
        else{
            b = xml_find_body(xsess, "session-id");
            if (b && session_id == atoi(b))
                cligen_output(stdout, "*");
            else
                cligen_output(stdout, " ");
            cligen_output(stdout, "%-8s",  b?b:"");
            b = xml_find_body(xsess, "username");
            cligen_output(stdout, "%-11s",  b?b:"");
            b = xml_find_body(xsess, "transport");
            cligen_output(stdout, "%-16s",  b?b:"");
            b = xml_find_body(xsess, "login-time");
            cligen_output(stdout, "%-16s",  b?b:"");
            cligen_output(stdout, "\n");
        }
    }
    retval = 0;
 done:
    if (nsc)
        cvec_free(nsc);
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Send a pull transient
 *
 * @param[in]   h    Clixon handle
 * @param[out]  tid  Transaction id
 * @retval      0    OK
 * @retval     -1    Error
 */
static int
send_pull_transient(clixon_handle h,
                    char         *name,
                    char        **tidstrp)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    cxobj     *xtop = NULL;
    cxobj     *xret = NULL;
    cxobj     *xrpc;
    cxobj     *xreply;
    cxobj     *xerr;
    cxobj     *xid;
    char      *tidstr = NULL;
    uint64_t   tid=0;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<config-pull xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", name);
    cprintf(cb, "<transient>true</transient>>");
    cprintf(cb, "</config-pull>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if ((xid = xpath_first(xreply, NULL, "tid")) == NULL){
        clixon_err(OE_CFG, 0, "No returned id");
        goto done;
    }
    if ((tidstr = strdup(xml_body(xid))) == NULL){
        clixon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    if (parse_uint64(tidstr, &tid, NULL) <= 0)
        goto done;
    if (tid == 0){
        clixon_err(OE_UNIX, errno, "Invalid tid = 0");
        goto done;
    }
    if (tidstrp && (*tidstrp = strdup(tidstr)) == NULL){
        clixon_err(OE_UNIX, errno, "strdup");
        goto done;
    }
    retval = 0;
 done:
    if (tidstr)
        free(tidstr);
    if (cb)
        cbuf_free(cb);
    if (xtop)
        xml_free(xtop);
    if (xret)
        xml_free(xret);
    return retval;
}

/*! Compare device config types: running with last saved synced or current device (transient)
 *
 * @param[in]   h       Clixon handle
 * @param[in]   cvv     name: device pattern
 * @param[in]   argv    <format>        "text"|"xml"|"json"|"cli"|"netconf" (see format_enum)
 * @param[in]   dt1     First device config config
 * @param[in]   dt2     Second device config config
 * @param[out]  cbdiff  Diff string to show
 * @retval      0       OK
 * @retval     -1       Error
 */
static int
compare_device_config_type(clixon_handle      h,
                           cvec              *cvv,
                           cvec              *argv,
                           device_config_type dt1,
                           device_config_type dt2,
                           cbuf              *cbdiff)
{
    int                retval = -1;
    enum format_enum   format;
    cg_var            *cv;
    char              *pattern = "*";
    char              *tidstr = NULL;
    char              *formatstr;
    cxobj             *xtop = NULL;
    cxobj             *xret = NULL;
    cxobj             *xrpc;
    cxobj             *xreply;
    cxobj             *xerr;
    cxobj             *xdiff;
    cbuf              *cb = NULL;
    char              *device_type = NULL;
    transaction_result result;
    cxobj            **vec = NULL;
    size_t             veclen;
    int                i;

    if (cvec_len(argv) > 1){
        clixon_err(OE_PLUGIN, EINVAL, "Received %d arguments. Expected: <format>]", cvec_len(argv));
        goto done;
    }
    if (cbdiff == NULL){
        clixon_err(OE_PLUGIN, EINVAL, "cbdiff is NULL");
        goto done;
    }
    cv = cvec_i(argv, 0);
    formatstr = cv_string_get(cv);
    if ((int)(format = format_str2int(formatstr)) < 0){
        clixon_err(OE_PLUGIN, 0, "Not valid format: %s", formatstr);
        goto done;
    }
    if ((cv = cvec_find(cvv, "name")) != NULL)
        pattern = cv_string_get(cv);
    /* If remote, start with requesting it asynchrously */
    if (dt1 == DT_TRANSIENT || dt2 == DT_TRANSIENT){
        /* Send pull <transient> */
        if (send_pull_transient(h, pattern, &tidstr) < 0)
            goto done;
        /* Wait to complete transaction try ^C here */
        if (transaction_notification_poll(h, tidstr, &result) < 0)
            goto done;
        if (result != TR_SUCCESS)
            goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<datastore-diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<xpath>config</xpath>");
    cprintf(cb, "<format>%s</format>", formatstr);
    device_type = device_config_type_int2str(dt1);
    cprintf(cb, "<devname>%s</devname>", pattern);
    cprintf(cb, "<config-type1>%s</config-type1>", device_type);
    device_type = device_config_type_int2str(dt2);
    cprintf(cb, "<config-type2>%s</config-type2>", device_type);
    cprintf(cb, "</datastore-diff>");
    cprintf(cb, "</rpc>");
    if (xtop){
        xml_free(xtop);
        xtop = NULL;
    }
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if (xpath_vec(xreply, NULL, "diff", &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        if ((xdiff = vec[i]) != NULL &&
            xml_body(xdiff) != NULL){
            cprintf(cbdiff, "%s", xml_body(xdiff));
        }
    }
    retval = 0;
 done:
    if (tidstr)
        free(tidstr);
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Compare datastores uses special diff rpc
 *
 * Use specialized rpc to reduce bandwidth
 * @param[in]   h     Clixon handle
 * @param[in]   cvv
 * @param[in]   argv  <db1> <db2> <format>
 * @retval      0     OK
 * @retval     -1     Error
 * @see compare_dbs  original function
 */
int
compare_dbs_rpc(clixon_handle h,
                cvec         *cvv,
                cvec         *argv)
{
    int     retval = -1;
    char   *db1;
    char   *db2;
    char   *formatstr;
    cxobj  *xtop = NULL;
    cxobj  *xret = NULL;
    cxobj  *xrpc;
    cxobj  *xreply;
    cxobj  *xerr;
    cxobj  *xdiff;
    cbuf   *cb = NULL;
    cxobj **vec = NULL;
    size_t  veclen;
    int     i;

    if (cvec_len(argv) != 3){
        clixon_err(OE_PLUGIN, EINVAL, "Expected arguments: <db1> <db2> <format>");
        goto done;
    }
    db1 = cv_string_get(cvec_i(argv, 0));
    db2 = cv_string_get(cvec_i(argv, 1));
    formatstr = cv_string_get(cvec_i(argv, 2));
    if (format_str2int(formatstr) < 0){
        clixon_err(OE_XML, 0, "format not found %s", formatstr);
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<datastore-diff xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<format>%s</format>", formatstr);
    cprintf(cb, "<dsref1>ds:%s</dsref1>", db1);
    cprintf(cb, "<dsref2>ds:%s</dsref2>", db2);
    cprintf(cb, "</datastore-diff>");
    cprintf(cb, "</rpc>");
    if (xtop){
        xml_free(xtop);
        xtop = NULL;
    }
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    if (xpath_vec(xreply, NULL, "diff", &vec, &veclen) < 0)
        goto done;
    for (i=0; i<veclen; i++){
        if ((xdiff = vec[i]) != NULL &&
            xml_body(xdiff) != NULL)
            cligen_output(stdout, "%s", xml_body(xdiff));
    }
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Compare device dbs: running with (last) synced db
 *
 * @param[in]   h     Clixon handle
 * @param[in]   cvv
 * @param[in]   argv  arg: 0 as xml, 1: as text
 * @retval      0     OK
 * @retval     -1     Error
 */
int
compare_device_db_sync(clixon_handle h,
                       cvec         *cvv,
                       cvec         *argv)
{
    int   retval = -1;
    cbuf *cbdiff = NULL;

    if ((cbdiff = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    if (compare_device_config_type(h, cvv, argv, DT_SYNCED, DT_RUNNING, cbdiff) < 0)
        goto done;
    if (strlen(cbuf_get(cbdiff)))
        cligen_output(stdout, "%s", cbuf_get(cbdiff));
    retval = 0;
 done:
    if (cbdiff)
        cbuf_free(cbdiff);
    return retval;
}

/*! Compare device dbs: running with current device (transient)
 *
 * @param[in] h     Clixon handle
 * @param[in] cvv  : name pattern or NULL
 * @param[in] argv  arg: 0 as xml, 1: as text
 * @retval    0     OK
 * @retval   -1     Error
 * @see check_device_db  only replies with boolean
 */
int
compare_device_db_dev(clixon_handle h,
                      cvec         *cvv,
                      cvec         *argv)
{
    int   retval = -1;
    cbuf *cbdiff = NULL;

    if ((cbdiff = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    if (compare_device_config_type(h, cvv, argv, DT_TRANSIENT, DT_RUNNING, cbdiff) < 0)
        goto done;
    if (strlen(cbuf_get(cbdiff)))
        cligen_output(stdout, "%s", cbuf_get(cbdiff));
    retval = 0;
 done:
    if (cbdiff)
        cbuf_free(cbdiff);
    return retval;
}

/*! Check if device(s) is in sync
 *
 * @param[in] h
 * @param[in] cvv  : name pattern
 * @param[in] argv
 * @retval    0    OK
 * @retval   -1    Error
 * @see compare_device_db_dev  with detailed diff
 */
int
check_device_db(clixon_handle h,
                cvec         *cvv,
                cvec         *argv)
{
    int   retval = -1;
    cbuf *cbdiff = NULL;

    if ((cbdiff = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    if (compare_device_config_type(h, cvv, argv, DT_RUNNING, DT_TRANSIENT, cbdiff) < 0)
        goto done;
    if (strlen(cbuf_get(cbdiff)))
        cligen_output(stdout, "device out-of-sync\n");
    else
        cligen_output(stdout, "OK\n");
    retval = 0;
 done:
    if (cbdiff)
        cbuf_free(cbdiff);
    return retval;
}

/*! Sub-routine for device dbxml: api-path to xml and send edit-config
 *
 * @param[in]  h     Clixon handle
 * @param[in]  cvv   Vector of cli string and instantiated variables
 * @param[in]  op    Operation to perform on datastore
 * @param[in]  nsctx Namespace context for last value added
 * @param[in]  api_path
 * @retval     0     OK
 * @retval    -1     Error
 */
static int
cli_dbxml_devs_sub(clixon_handle       h,
                   cvec               *cvv,
                   enum operation_type op,
                   cvec               *nsctx,
                   int                 cvvi,
                   char               *api_path)
{
    int        retval = -1;
    cxobj     *xtop = NULL;     /* xpath root */
    cxobj     *xbot = NULL;     /* xpath, NULL if datastore */
    cxobj     *xerr = NULL;
    yang_stmt *yspec0;
    yang_stmt *yspec;
    cbuf      *cb = NULL;
    yang_stmt *y = NULL;        /* yang spec of xpath */
    cg_var    *cv;
    int        ret;

    /* Top-level yspec */
    if ((yspec0 = clicon_dbspec_yang(h)) == NULL){
        clixon_err(OE_FATAL, 0, "No DB_SPEC");
        goto done;
    }
    /* Create config top-of-tree */
    if ((xtop = xml_new(NETCONF_INPUT_CONFIG, NULL, CX_ELMNT)) == NULL)
        goto done;
    xbot = xtop;
    if (api_path){
        if ((ret = api_path2xml(api_path, yspec0, xtop, YC_DATANODE, 1, &xbot, &y, &xerr)) < 0)
            goto done;
        if (ret == 0){
            clixon_err_netconf(h, OE_CFG, EINVAL, xerr, "api-path syntax error \"%s\": ", api_path);
            goto done;
        }
    }
    if (xml_add_attr(xbot, "operation", xml_operation2str(op), NETCONF_BASE_PREFIX, NULL) == NULL)
        goto done;
    /* Add body last in case of leaf */
    if (cvec_len(cvv) > 1 &&
        (yang_keyword_get(y) == Y_LEAF)){
        /* Add the body last if there is remaining element that was not used in the
         * earlier api-path transformation.
         * This is to handle differences between:
         * DELETE <foo>bar</foo> and DELETE <foo/>
         * i.e., (1) deletion of a specific leaf entry vs (2) deletion of any entry
         * Discussion: one can claim (1) is "bad" usage but one could see cases where
         * you would want to delete a value if it has a specific value but not otherwise
         */
        if (cvvi != cvec_len(cvv)){
            if (dbxml_body(xbot, cvv) < 0)
                goto done;
        }
        /* Loop over namespace context and add them to this leaf node */
        cv = NULL;
        while ((cv = cvec_each(nsctx, cv)) != NULL){
            char *ns = cv_string_get(cv);
            char *pf = cv_name_get(cv);
            if (ns && pf && xmlns_set(xbot, pf, ns) < 0)
                goto done;
        }
    }
    /* Special handling of identityref:s whose body may be: <namespace prefix>:<id>
     * Ensure the namespace is declared if it exists in YANG
     */
    if (y)
        yspec =  ys_spec(y);
    else
        yspec = yspec0;
    if ((ret = xml_apply0(xbot, CX_ELMNT, identityref_add_ns, yspec)) < 0)
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    if (clixon_xml2cbuf(cb, xtop, 0, 0, NULL, -1, 0) < 0)
        goto done;
    if (clicon_rpc_edit_config(h, "candidate", OP_NONE, cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xtop)
        xml_free(xtop);
    if (xerr)
        xml_free(xerr);
    return retval;
}

/*! Modify xml datastore from a callback using xml key format strings
 *
 * @param[in]  h     Clixon handle
 * @param[in]  cvv   Vector of cli string and instantiated variables
 * @param[in]  argv  Vector: <apipathfmt> [<mointpt>], eg "/aaa/%s"
 * @param[in]  op    Operation to perform on datastore
 * @param[in]  nsctx Namespace context for last value added
 * @retval     0     OK
 * @retval    -1     Error
 * cvv first contains the complete cli string, and then a set of optional
 * instantiated variables.
 * If the last node is a leaf, the last cvv element is added as a value. This value
 * Example:
 * cvv[0]  = "set interfaces interface eth0 type bgp"
 * cvv[1]  = "eth0"
 * cvv[2]  = "bgp"
 * argv[0] = "/interfaces/interface/%s/type"
 * op: OP_MERGE
 * @see cli_callback_generate where arg is generated
 * @note The last value may require namespace binding present in nsctx. Note that the nsctx
 *   cannot normally be supplied by the clispec functions, such as cli_set, but need to be
 *   generated by a function such as clixon_instance_id_bind() or other programmatically.
 * @see cli_show_auto_devs Similar controller handling
 */
static int
cli_dbxml_devs(clixon_handle       h,
               cvec               *cvv,
               cvec               *argv,
               enum operation_type op,
               cvec               *nsctx)
{
    int        retval = -1;
    char      *api_path_fmt;    /* xml key format */
    char      *api_path_fmt01 = NULL;
    char      *api_path = NULL;
    cg_var    *cv;
    int        cvvi = 0;
    char      *mtpoint = NULL;
    char      *pattern;
    cxobj     *xdevs = NULL;
    cxobj     *xdev;
    char      *devname;
    int        devices = 0;
    char      *str;
    cbuf      *api_path_fmt_cb = NULL;    /* xml key format */
    int        i;

    if (cvec_len(argv) < 1){
        clixon_err(OE_PLUGIN, EINVAL, "Requires first element to be xml key format string");
        goto done;
    }
    if ((api_path_fmt_cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Remove all keywords */
    if (cvec_exclude_keys(cvv) < 0)
        goto done;
    /* Concatenate all argv strings to a single string
     * Variant of cvec_concat_cb() where api-path-fmt may be interleaved with mtpoint,
     * eg /api-path-fmt2 mtpoint /api-path-fmt1 /api-path-fmt0
     */
    for (i=cvec_len(argv)-1; i>=0; i--){
        cv = cvec_i(argv, i);
        str = cv_string_get(cv);
        if (str[0] != '/')
            continue;
        cprintf(api_path_fmt_cb, "%s", str);
    }
    api_path_fmt = cbuf_get(api_path_fmt_cb);
    /* See if 2nd arg is mountpoint and if devices cmd tree is selected */
    if (cvec_len(argv) > 1 &&
        (cv = cvec_i(argv, 1)) != NULL &&
        (str = cv_string_get(cv)) != NULL &&
        strncmp(str, "mtpoint:", strlen("mtpoint:")) == 0){
        mtpoint = str + strlen("mtpoint:");
        devices = strstr(mtpoint, "/ctrl:devices") != NULL;
    }
    else{
        devices = strstr(api_path_fmt, "/clixon-controller:devices") != NULL;
    }
    if (devices && (cv = cvec_find(cvv, "name")) != NULL){
        pattern = cv_string_get(cv);
        if (rpc_get_yanglib_mount_match(h, pattern, 0, 0, &xdevs) < 0)
            goto done;
        if (xdevs == NULL){
            if (cli_apipath(h, cvv, mtpoint, api_path_fmt, &cvvi, &api_path) < 0)
                goto done;
            if (cli_dbxml_devs_sub(h, cvv, op, nsctx, cvvi, api_path) < 0)
                goto done;
        }
        else {
            xdev = NULL;
            while ((xdev = xml_child_each(xdevs, xdev, CX_ELMNT)) != NULL) {
                if ((devname = xml_find_body(xdev, "name")) == NULL)
                    continue;
                cv_string_set(cv, devname); /* replace name */
                if (cli_apipath(h, cvv, mtpoint, api_path_fmt, &cvvi, &api_path) < 0)
                    goto done;
                if (cli_dbxml_devs_sub(h, cvv, op, nsctx, cvvi, api_path) < 0)
                    goto done;
                if (api_path){
                    free(api_path);
                    api_path = NULL;
                }
            }
        }
    }
    else{
        if (cli_apipath(h, cvv, mtpoint, api_path_fmt, &cvvi, &api_path) < 0)
            goto done;
        if (cli_dbxml_devs_sub(h, cvv, op, nsctx, cvvi, api_path) < 0)
            goto done;
    }
    retval = 0;
 done:
    if (api_path_fmt_cb)
        cbuf_free(api_path_fmt_cb);
    if (api_path_fmt01)
        free(api_path_fmt01);
    if (api_path)
        free(api_path);
    return retval;
}

/*! CLI callback: set auto db item, specialization for controller devices
 *
 * @param[in]  h    Clixon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 * Format of argv:
 *   <api-path-fmt> Generated
 * @see cli_auto_set  original callback
 */
int
cli_auto_set_devs(clixon_handle h,
                  cvec         *cvv,
                  cvec         *argv)
{
    int   retval = -1;
    cvec *cvv2 = NULL;

    cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv);
    if (cli_dbxml_devs(h, cvv2, argv, OP_REPLACE, NULL) < 0)
        goto done;
    retval = 0;
 done:
    if (cvv2)
        cvec_free(cvv2);
    return retval;
}

/*! Merge datastore xml entry, specialization for controller devices
 *
 * @param[in]  h    Clixon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 * @see cli_auto_merge  original callback
 */
int
cli_auto_merge_devs(clixon_handle h,
                    cvec         *cvv,
                    cvec         *argv)
{
    int retval = -1;
    cvec *cvv2 = NULL;

    cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv);
    if (cli_dbxml_devs(h, cvv2, argv, OP_MERGE, NULL) < 0)
        goto done;
    retval = 0;
 done:
    if (cvv2)
        cvec_free(cvv2);
    return retval;
}

/*! Delete datastore xml, specialization for controller devices
 *
 * @param[in]  h    Clixon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 * @see cli_auto_del  original callback
 */
int
cli_auto_del_devs(clixon_handle h,
                  cvec         *cvv,
                  cvec         *argv)
{
    int   retval = -1;
    cvec *cvv2 = NULL;

    cvv2 = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv);
    if (cli_dbxml_devs(h, cvv2, argv, OP_REMOVE, NULL) < 0)
        goto done;
    retval = 0;
 done:
    if (cvv2)
        cvec_free(cvv2);
    return retval;
}

/*! Merge datastore xml entry, specialization for controller devices
 *
 * @param[in]  h    Clixon handle
 * @param[in]  cvv0 Vector of cli string and instantiated variables
 * @param[in]  argv Vector. First element xml key format string, eg "/aaa/%s"
 * @see cli_auto_merge  original callback
 */
int
cli_auto_load_devs(clixon_handle h,
                   cvec         *cvv0,
                   cvec         *argv)
{
    int                 retval = -1;
    enum operation_type op = OP_MERGE;
    enum format_enum    format = FORMAT_XML;
    cg_var             *cv;
    cvec               *cvv = NULL;
    char               *filename = NULL;
    FILE               *fp = stdin;
    struct stat         st;
    cxobj              *xt = NULL;
    cxobj              *xerr = NULL;
    cbuf               *cb = NULL;
    int                 ret;

    if ((cvv = cvec_append(clicon_data_cvec_get(h, "cli-edit-cvv"), cvv0)) == NULL)
        goto done;
    if ((cv = cvec_find(cvv, "operation")) != NULL) {
        if (xml_operation(cv_string_get(cv), &op) < 0)
            goto done;
    }
    if ((cv = cvec_find(cvv, "format")) != NULL) {
        if ((format = format_str2int(cv_string_get(cv))) < 0)
            goto done;
    }
    if ((cv = cvec_find(cvv, "filename")) != NULL){
        filename = cv_string_get(cv);
        if (stat(filename, &st) < 0){
            clixon_err(OE_UNIX, errno, "load_config: stat(%s)", filename);
            goto done;
        }
        /* Open and parse local file into xml */
        if ((fp = fopen(filename, "r")) == NULL){
            clixon_err(OE_UNIX, errno, "fopen(%s)", filename);
            goto done;
        }
    }
    /* XXX Do without YANG (for the time being) */
    switch (format){
    case FORMAT_XML:
        if ((ret = clixon_xml_parse_file(fp, YB_NONE, NULL, &xt, &xerr)) < 0)
            goto done;
        if (ret == 0){
            clixon_err_netconf(h, OE_XML, 0, xerr, "Loading: %s", filename?filename:"stdin");
            goto done;
        }
        if (xml_child_nr(xt) == 0){
            clixon_err(OE_XML, 0, "No XML in file %s", filename?filename:"stdin");
            goto done;
        }
        break;
    case FORMAT_JSON:
        if ((ret = clixon_json_parse_file(fp, 1, YB_NONE, NULL, &xt, &xerr)) < 0)
            goto done;
        if (ret == 0){
            clixon_err_netconf(h, OE_XML, 0, xerr, "Loading: %s", filename?filename:"stdin");
            goto done;
        }
        if (xml_child_nr(xt) == 0){
            clixon_err(OE_XML, 0, "No XML in file %s", filename?filename:"stdin");
            goto done;
        }
        break;
    default:
        clixon_err(OE_PLUGIN, 0, "format: %s not implemented", format_int2str(format));
        goto done;
        break;
    }
    if (xt == NULL)
        goto done;
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (clixon_xml2cbuf(cb, xt, 0, 0, NULL, -1, 1) < 0)
        goto done;
    if (clicon_rpc_edit_config(h, "candidate",
                               op,
                               cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xt)
        xml_free(xt);
    if (xerr)
        xml_free(xerr);
    if (fp){
        if (filename)
            fclose(fp);
        else
            clearerr(fp);
    }
    if (cvv)
        cvec_free(cvv);
    return retval;
}

/*! Show controller and clixon version
 */
int
cli_controller_show_version(clixon_handle h,
                            cvec         *vars,
                            cvec         *argv)
{
    cligen_output(stdout, "Controller: \t%s\n", CONTROLLER_VERSION);
    cligen_output(stdout, "Clixon: \t%s\n", CLIXON_VERSION_STRING);
    cligen_output(stdout, "CLIgen: \t%s\n", CLIGEN_VERSION);
    return 0;
}

/*! Apply template on devices
 *
 * @param[in] h
 * @param[in] cvv  templ, devs
 * @param[in] argv null
 * @retval    0    OK
 * @retval   -1    Error
 */
int
cli_apply_device_template(clixon_handle h,
                          cvec         *cvv,
                          cvec         *argv)
{
    int     retval = -1;
    cbuf   *cb = NULL;
    cg_var *cv;
    cxobj  *xtop = NULL;
    cxobj  *xrpc;
    cxobj  *xret = NULL;
    cxobj  *xreply;
    cxobj  *xerr;
    char   *devs = "*";
    char   *templ;
    char   *var;

    if (argv != NULL){
        clixon_err(OE_PLUGIN, EINVAL, "requires expected NULL");
        goto done;
    }
    if ((cv = cvec_find(cvv, "templ")) == NULL){
        clixon_err(OE_PLUGIN, EINVAL, "template variable required");
        goto done;
    }
    templ = cv_string_get(cv);
    if ((cv = cvec_find(cvv, "devs")) != NULL)
        devs = cv_string_get(cv);
    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" username=\"%s\" %s>",
            NETCONF_BASE_NAMESPACE,
            clicon_username_get(h),
            NETCONF_MESSAGE_ID_ATTR);
    cprintf(cb, "<device-template-apply xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    cprintf(cb, "<devname>%s</devname>", devs);
    cprintf(cb, "<template>%s</template>", templ);
    cprintf(cb, "<variables>");
    cv = NULL;
    while ((cv = cvec_each(cvv, cv)) != NULL){
        if (strcmp(cv_name_get(cv), "var") == 0){
            var = cv_string_get(cv);
            if ((cv = cvec_next(cvv, cv)) == NULL)
                break;
            if (strcmp(cv_name_get(cv), "val") == 0){
                cprintf(cb, "<variable><name>%s</name><value>%s</value></variable>",
                        var, cv_string_get(cv));
            }
        }
    }
    cprintf(cb, "</variables>");
    cprintf(cb, "</device-template-apply>");
    cprintf(cb, "</rpc>");
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, &xtop, NULL) < 0)
        goto done;
    /* Skip top-level */
    xrpc = xml_child_i(xtop, 0);
    /* Send to backend */
    if (clicon_rpc_netconf_xml(h, xrpc, &xret, NULL) < 0)
        goto done;
    if ((xreply = xpath_first(xret, NULL, "rpc-reply")) == NULL){
        clixon_err(OE_CFG, 0, "Malformed rpc reply");
        goto done;
    }
    if ((xerr = xpath_first(xreply, NULL, "rpc-error")) != NULL){
        clixon_err_netconf(h, OE_XML, 0, xerr, "Get configuration");
        goto done;
    }
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    if (xret)
        xml_free(xret);
    if (xtop)
        xml_free(xtop);
    return retval;
}
