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
  Device connection state machine:

 CS_CLOSED \
     ^      \ connect  
     |       v        send get
     |<-- CS_CONNECTING
     |       |
     |       v
     |<-- CS_SCHEMA_LIST
     |       |       \
     |       |        v
     |<-------- CS_SCHEMA_ONE(n) ---+
     |       |       /           <--+
     |       v      v       
     |<-- CS_DEVICE_SYNC
     |      / 
     |     /  
 CS_OPEN <+

 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <syslog.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/time.h>

/* clicon */
#include <cligen/cligen.h>

/* Clicon library functions. */
#include <clixon/clixon.h>

/* These include signatures for plugin and transaction callbacks. */
#include <clixon/clixon_backend.h>

/* Controller includes */
#include "controller_custom.h"
#include "controller.h"
#include "controller_netconf.h"
#include "controller_device_state.h"
#include "controller_device_handle.h"

/*! Mapping between enum conn_state and yang connection-state
 * @see clixon-controller@2023-01-01.yang connection-state
 */
static const map_str2int csmap[] = {
    {"CLOSED",       CS_CLOSED},
    {"CONNECTING",   CS_CONNECTING},
    {"SCHEMA_LIST",  CS_SCHEMA_LIST},
    {"SCHEMA_ONE",   CS_SCHEMA_ONE}, /* substate is schema-nr */
    {"DEVICE-SYNC",  CS_DEVICE_SYNC},
    {"OPEN",         CS_OPEN},
    {"WRESP",        CS_WRESP},
    {NULL,           -1}
};

/*! Mapping between enum config_state and yang config-state
 * @see clixon-controller@2023-01-01.yang config-state
 */
static const map_str2int cfmap[] = {
    {"CLOSED",     CF_CLOSED},
    {"YANG",       CF_YANG},
    {"VALIDATE",   CF_VALIDATE},
    {NULL,         -1}
};

/*! Map controller device connection state from int to string 
 * @param[in]  state  Device state as int
 * @retval     str    Device state as string
 */
char *
device_state_int2str(conn_state_t state)
{
    return (char*)clicon_int2str(csmap, state);
}

/*! Map controller device connection state from string to int 
 * @param[in]  str    Device state as string
 * @retval     state  Device state as int
 */
conn_state_t
device_state_str2int(char *str)
{
    return clicon_str2int(csmap, str);
}

/*! Map controller device connection state from string to int 
 * @param[in]  str    Device state as string
 * @retval     state  Device state as int
 */
enum config_state
config_state_str2int(char *str)
{
    return clicon_str2int(cfmap, str);
}

/*! Close connection, unregister events and timers
 * @param[in]  dh      Clixon client handle.
 * @param[in]  format  Format string for Log message
 * @retval     0       OK
 * @retval    -1       Error
 */
int
device_close_connection(device_handle dh,
                        const char   *format, ...)
{
    int            retval = -1;
    va_list        ap;
    size_t         len;
    char          *str = NULL;
    int            s;
    
    s = device_handle_socket_get(dh);
    clixon_event_unreg_fd(s, device_input_cb); /* deregister events */
    device_state_timeout_unregister(dh);
    device_handle_disconnect(dh);              /* close socket, reap sub-processes */
    device_handle_yang_lib_set(dh, NULL);
    device_handle_conn_state_set(dh, CS_CLOSED);
    if (format == NULL)
        device_handle_logmsg_set(dh, NULL);
    else {
        va_start(ap, format); /* dryrun */
        if ((len = vsnprintf(NULL, 0, format, ap)) < 0) /* dryrun, just get len */
            goto done;
        va_end(ap);
        if ((str = malloc(len+1)) == NULL){
            clicon_err(OE_UNIX, errno, "malloc");
            goto done;
        }
        va_start(ap, format); /* real */
        if (vsnprintf(str, len+1, format, ap) < 0){
            clicon_err(OE_UNIX, errno, "vsnprintf");
            goto done;
        }
        va_end(ap);
        device_handle_logmsg_set(dh, str);
        clicon_debug(1, "%s %s: %s", __FUNCTION__, device_handle_name_get(dh), str);
    }
    retval = 0;
 done:
    return retval;
}

/*! Handle input data from device, whole or part of a frame ,called by event loop
 * @param[in] s   Socket
 * @param[in] arg Device handle
 */
int
device_input_cb(int   s,
                void *arg)
{
    device_handle dh = (device_handle)arg;
    clixon_handle h;
    int           retval = -1;
    int           eom = 0;
    int           eof = 0;
    int           frame_state; /* only used for chunked framing not eom */
    size_t        frame_size;
    cbuf         *cb;
    yang_stmt    *yspec;
    cxobj        *xtop = NULL;
    cxobj        *xmsg;
    int           ret;
    char         *name;

    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    h = device_handle_handle_get(dh);
    frame_state = device_handle_frame_state_get(dh);
    frame_size = device_handle_frame_size_get(dh);
    cb = device_handle_frame_buf_get(dh);
    name = device_handle_name_get(dh);
    /* Read data, if eom set a frame is read
     */
    if (netconf_input_msg(s,
                          clicon_option_int(h, "netconf-framing"),
                          &frame_state, &frame_size,
                          cb, &eom, &eof) < 0)
        goto done;
    if (eof){         /* Close connection, unregister events, free mem */
        clicon_debug(1, "%s %s: eom:%d eof:%d len:%lu Remote socket endpoint closed", __FUNCTION__,
                     name, eom, eof, cbuf_len(cb));
        device_close_connection(dh, "Remote socket endpoint closed");
        goto ok;
    }
    device_handle_frame_state_set(dh, frame_state);
    device_handle_frame_size_set(dh, frame_size);
    if (eom == 0){ /* frame not complete */
        clicon_debug(CLIXON_DBG_DETAIL, "%s %s: frame: %lu strlen:%lu", __FUNCTION__,
                     name, cbuf_len(cb), strlen(cbuf_get(cb)));
        goto ok;
    }
    clicon_debug(1, "%s %s: frame: %lu strlen:%lu", __FUNCTION__,
                 name, cbuf_len(cb), strlen(cbuf_get(cb)));
    cbuf_trunc(cb, cbuf_len(cb));
    clicon_debug(CLIXON_DBG_MSG, "Recv dev: %s", cbuf_get(cb));
    yspec = clicon_dbspec_yang(h);
    if ((ret = netconf_input_frame(cb, yspec, &xtop)) < 0)
        goto done;
    cbuf_reset(cb);
    if (ret==0){
        device_close_connection(dh, "Invalid frame");
        goto ok;
    }
    xmsg = xml_child_i_type(xtop, 0, CX_ELMNT);
    if (xmsg && device_state_handler(h, dh, s, xmsg) < 0)
        goto done;
 ok:
    retval = 0;
 done:
    clicon_debug(CLIXON_DBG_DETAIL, "%s retval:%d", __FUNCTION__, retval);
    if (xtop)
        xml_free(xtop);
    return retval;
}

/*! Send a <get> request to a device
 *
 * @param[in]  h   Clixon handle
 * @param[in]  dh  Clixon client handle
 * @param[in]  s   Socket
 * @retval     0   OK
 * @retval    -1   Error
 */
int
device_send_sync(clixon_handle h,
                 device_handle dh,
                 int           s)
{
    int   retval = -1;
    cbuf *cb = NULL;
    int   encap;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE, 
            device_handle_msg_id_getinc(dh));
    if (1) { // get-config
        cprintf(cb, "<get-config>");
        cprintf(cb, "<source><running/></source>");
        cprintf(cb, "</get-config>");
    }
    else { // get
        cprintf(cb, "<get>");
        cprintf(cb, "</get>");
    }
    cprintf(cb, "</rpc>");
    encap = clicon_option_int(h, "netconf-framing");
    if (netconf_output_encap(encap, cb) < 0)
        goto done;
    s = device_handle_socket_get(dh);
    if (clicon_msg_send1(s, cb) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Send s single get-schema requests to a device
 *
 * @param[in]  h   Clixon handle
 * @param[in]  dh  Clixon client handle
 * @param[in]  s   Socket
 * @param[in]  xd  XML tree of netconf monitor schema entry
 * @retval     0   OK, sent a get-schema request
 * @retval    -1   Error
 * @see ietf-netconf-monitoring@2010-10-04.yang
 */
static int
device_get_schema_sendit(clixon_handle h,
                         device_handle dh,
                         int           s,
                         char         *identifier,
                         char         *version)
{
    int      retval = -1;
    cbuf    *cb = NULL;
    uint64_t seq;
    int      encap;
    char    *name;
    
    name = device_handle_name_get(dh);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    seq = device_handle_msg_id_getinc(dh);
    cprintf(cb, "<rpc xmlns=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE, seq);
    cprintf(cb, "<get-schema xmlns=\"%s\">", NETCONF_MONITORING_NAMESPACE);
    cprintf(cb, "<identifier>%s</identifier>", identifier);
    cprintf(cb, "<version>%s</version>", version);
    cprintf(cb, "<format>yang</format>");
    cprintf(cb, "</get-schema>");
    cprintf(cb, "</rpc>");
    encap = clicon_option_int(h, "netconf-framing");
    if (netconf_output_encap(encap, cb) < 0)
        goto done;
    if (clicon_msg_send1(s, cb) < 0)
        goto done;
    clicon_debug(1, "%s %s: sent get-schema(%s@%s) seq:%" PRIu64, __FUNCTION__, name, identifier, version, seq);
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Send next get-schema requests to a device
 *
 * @param[in]     h   Clixon handle
 * @param[in]     dh  Clixon client handle
 * @param[in]     s   Socket
 * @param[in,out] nr  Last schema index sent
 * @retval        1   Sent a get-schema, nr updated
 * @retval        0   No schema sent, either because all are sent or they are none
 * @retval       -1   Error
 */
static int
device_send_get_schema_next(clixon_handle h,
                            device_handle dh,
                            int           s,
                            int          *nr)
{
    int        retval = -1;
    int        ret;
    cxobj     *xylib;
    cxobj     *x;
    char      *name;
    char      *revision;
    yang_stmt *yspec;
    cxobj     **vec = NULL;
    size_t      veclen;
    cvec     *nsc = NULL;
    int        i;

    clicon_debug(CLIXON_DBG_DETAIL, "%s %d", __FUNCTION__, *nr);
    if ((yspec = device_handle_yspec_get(dh)) == NULL){
        clicon_err(OE_YANG, 0, "No yang spec");
        goto done;
    }
    xylib = device_handle_yang_lib_get(dh);
    x = NULL;
    if (xpath_vec(xylib, nsc, "module-set/module", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        x = vec[i];
        if (i != *nr)
            continue;
        name = xml_find_body(x, "name");
        revision = xml_find_body(x, "revision");
        (*nr)++;
        /* Check if already loaded */
        if (yang_find_module_by_name_revision(yspec, name, revision) != NULL)
            continue;
        /* Chekc if exists as local file */
        if ((ret = yang_file_find_match(h, name, revision, NULL)) < 0)
            goto done;
        if (ret == 1)
            continue;
        if ((ret = device_get_schema_sendit(h, dh, s, name, revision)) < 0)
            goto done;
        device_handle_schema_name_set(dh, name);
        device_handle_schema_rev_set(dh, revision);
        break;
    }
    if (i < veclen)
        retval = 1;
    else
        retval = 0;
 done:
    if (vec)
        free(vec);
    return retval;
}

/*! Send ietf-netconf-monitoring schema get request to get list of schemas
 *
 * @param[in]  h       Clixon handle.
 * @param[in]  dh      Clixon client handle.
 * @note this could be part of the generic sync, but juniper seems to need 
 * an explicit to target the schemas (and only that)
 */
static int
device_send_get_schema_list(clixon_handle h,
                            device_handle dh,
                            int           s)

{
    int   retval = -1;
    cbuf *cb = NULL;
    int   encap;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_PLUGIN, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<rpc xmlns=\"%s\" message-id=\"%" PRIu64 "\">",
            NETCONF_BASE_NAMESPACE, 
            device_handle_msg_id_getinc(dh));
    cprintf(cb, "<get>");
    cprintf(cb, "<filter type=\"subtree\">");
    cprintf(cb, "<netconf-state xmlns=\"%s\">", NETCONF_MONITORING_NAMESPACE);
    cprintf(cb, "<schemas/>");
    cprintf(cb, "</netconf-state>");
    cprintf(cb, "</filter>");
    cprintf(cb, "</get>");
    cprintf(cb, "</rpc>");
    encap = clicon_option_int(h, "netconf-framing");
    if (netconf_output_encap(encap, cb) < 0)
        goto done;
    if (clicon_msg_send1(s, cb) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Receive hello from device, send hello
 *
 * @param[in] h          Clixon handle.
 * @param[in] dh         Clixon client handle.
 * @param[in] s          Socket where input arrives. Read from this.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] rpcname    Name of RPC, only "hello" is expected here
 * @param[in] conn_state Device connection state, should be CONNECTING
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
static int
device_state_recv_hello(clixon_handle h,
                        device_handle dh,
                        int           s,
                        cxobj        *xmsg,
                        char         *rpcname,
                        conn_state_t  conn_state)
{
    int     retval = -1;
    char   *rpcprefix;
    char   *namespace = NULL;
    int     version;
    cvec   *nsc = NULL;
    cxobj  *xcaps;

    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    rpcprefix = xml_prefix(xmsg);
    if (xml2ns(xmsg, rpcprefix, &namespace) < 0)
        goto done;
    if (strcmp(rpcname, "hello") != 0){
        device_close_connection(dh, "Unexpected msg %s in state %s",
                   rpcname, device_state_int2str(conn_state));
        goto closed;
    }
    if (namespace == NULL || strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        device_close_connection(dh,  "No appropriate namespace associated with %s",
                   namespace);
        goto closed;
    }
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    if ((xcaps = xpath_first(xmsg, nsc, "/hello/capabilities")) == NULL){
        clicon_err(OE_PROTO, ESHUTDOWN, "No capabilities found");
        goto done;
    }
    /* Destructive, actually move subtree from xmsg */
    if (xml_rm(xcaps) < 0)
        goto done;
    if (device_handle_capabilities_set(dh, xcaps) < 0)
        goto done;
    /* Set NETCONF version */
    if (device_handle_capabilities_find(dh, "urn:ietf:params:netconf:base:1.1"))
        version = 1;
    else if (device_handle_capabilities_find(dh, "urn:ietf:params:netconf:base:1.0"))
        version = 0;
    else{
        device_close_connection(dh,  "No base netconf capability found");
        goto closed;
    }
    clicon_debug(1, "%s version: %d", __FUNCTION__, version);
    version = 0; /* XXX hardcoded to 0 */
    clicon_option_int_set(h, "netconf-framing", version);
    /* Send hello */
    if (clixon_client_hello(s, version) < 0)
        goto done;
    retval = 1;
 done:
   if (nsc)
       cvec_free(nsc);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Given devicename and XML tree, create XML tree and device mount-point
 *
 * @param[in]  devicename Name of device
 * @param[in]  yspec      Top level Yang spec
 * @param[out] xt         Top of created tree (Deallocate with xml_free)
 * @param[out] xroot      XML mount-point in created tree
 */
static int
device_state_mount_point_get(char      *devicename,
                             yang_stmt *yspec,
                             cxobj    **xtp,
                             cxobj    **xrootp)
{
    int    retval = -1;
    int    ret;
    cbuf  *cb = NULL;
    cxobj *xt = NULL;
    cxobj *xroot;
    
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<devices xmlns=\"%s\" xmlns:nc=\"%s\"><device><name>%s</name>",
            CONTROLLER_NAMESPACE,
            NETCONF_BASE_NAMESPACE, /* needed for nc:op below */
            devicename);
    cprintf(cb, "<root/>");
    cprintf(cb, "</device></devices>");
    if ((ret = clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, &xt, NULL)) < 0)
        goto done;
    if (xml_name_set(xt, "config") < 0)
        goto done;
    if ((xroot = xpath_first(xt, NULL, "devices/device/root")) == NULL){
        clicon_err(OE_XML, 0, "device/root mountpoint not found");
        goto done;
    }
    *xtp = xt;
    *xrootp = xroot;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Receive config data from device and add config to mount-point
 *
 * @param[in] h          Clixon handle.
 * @param[in] dh         Clixon client handle.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] yspec      Yang top-level spec
 * @param[in] rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in] conn_state Device connection state, should be WRESP
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
static int
device_state_recv_config(clixon_handle h,
                         device_handle dh,
                         cxobj        *xmsg,
                         yang_stmt    *yspec0,
                         char         *rpcname,
                         conn_state_t  conn_state)
{
    int       retval = -1;
    char      *rpcprefix;
    char      *namespace = NULL;
    cxobj     *xdata;
    cxobj     *xt;
    cxobj     *xa;
    cbuf      *cbret = NULL;
    cbuf      *cberr = NULL;
    char      *name;
    cvec      *nsc = NULL;
    yang_stmt *yspec1;
    int        ret;
    cxobj     *x;
    cxobj     *xroot;
    yang_stmt *yroot;
    cxobj     *xerr = NULL;
    config_state_t config_state;

    clicon_debug(1, "%s", __FUNCTION__);
    //    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    if (strcmp(rpcname, "rpc-reply") != 0){
        device_close_connection(dh, "Unexpected msg %s in state %s",
                                rpcname, device_state_int2str(conn_state));
        goto closed;
    }
    name = device_handle_name_get(dh);
    /* get namespace context from rpc-reply */
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    rpcprefix = xml_prefix(xmsg);
    if ((namespace = xml_nsctx_get(nsc, rpcprefix)) == NULL ||
        strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        device_close_connection(dh, "No appropriate namespace associated with:%s",
                                namespace);
        goto closed;
    }
    xdata = xpath_first(xmsg, nsc, "data");
    /* Move all xmlns declarations to <data> */
    if (xmlns_set_all(xdata, nsc) < 0)
        goto done;
    xml_sort(xdata);
    /* Create config tree (xt) and device mount-point (xroot) */
    if (device_state_mount_point_get(name, yspec0, &xt, &xroot) < 0)
        goto done;
    yroot = xml_spec(xroot);
    /* Sanity-check mount-point extension */
    if ((ret = yang_schema_mount_point(yroot)) < 0)
        goto done;
    if (ret == 0){
        clicon_err(OE_YANG, 0, "Device root is not a YANG schema mount-point");
        goto done;
    }
    if ((yspec1 = device_handle_yspec_get(dh)) == NULL){
        device_close_connection(dh, "No YANGs available");
        goto closed;
    }
    /*
     * <root>  clixon-controller:root
     * <data>  ietf-netconf:data (dont bother to bind this node, its just a placeholder)
     * <x>     bind to yspec1
     */
    if ((ret = xml_bind_yang(h, xdata, YB_MODULE, yspec1, &xerr)) < 0)
        goto done;
    if (ret == 0){
        if ((cberr = cbuf_new()) == NULL){
            clicon_err(OE_UNIX, errno, "cbuf_new");
            goto done;
        }
        cprintf(cberr, "YANG binding failed at mountpoint:");
        if ((x=xpath_first(xerr, NULL, "//error-message"))!=NULL)
            cprintf(cberr, "%s", xml_body(x));
        if (device_close_connection(dh, "%s", cbuf_get(cberr)) < 0)
            goto done;
        goto closed;
    }
    /* Add all xdata children to xroot
     * XXX:
     * 1. idempotent?
     * 2. should state nodes be added (they do now)?
     */
    while ((x = xml_child_i_type(xdata, 0, CX_ELMNT)) != NULL) {
        if (xml_addsub(xroot, x) < 0)
            goto done;
    }
    if (xml_sort_recurse(xroot) < 0)
        goto done;
#if 1
    //    clicon_debug_xml(1, x1, "mount-point");
#endif
    if ((cbret = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    /* Add op=OP_REPLACE to root mountpoint */
    if ((xa = xml_new("operation", xroot, CX_ATTR)) == NULL)
        goto done;
    if (xml_prefix_set(xa, NETCONF_BASE_PREFIX) < 0)
        goto done;
    // XXX: merge?
    if (xml_value_set(xa, xml_operation2str(OP_REPLACE)) < 0)
        goto done;
    if ((ret = xmldb_put(h, "candidate", OP_NONE, xt, NULL, cbret)) < 0)
        goto done;
    /* Check config-state */
    config_state = device_handle_config_state_get(dh);
    if (ret && (ret = candidate_commit(h, NULL, "candidate", 0,
                                       config_state==CF_VALIDATE?VL_FULL:VL_NONE,
                                       cbret)) < 0)
        goto done;
    if (ret == 0){ /* discard */
        xmldb_copy(h, "running", "candidate");            
        xmldb_modified_set(h, "candidate", 0); /* reset dirty bit */
        clicon_debug(CLIXON_DBG_DEFAULT, "%s", cbuf_get(cbret));
        if (device_close_connection(dh, 
#if 0
                                    /* XXX cbret is XML and looks ugly in logmsg (at least encode it?)*/
                                    "Failed to commit: %s", cbuf_get(cbret)
#else
                                    "Failed to commit"
#endif
                                    ) < 0)
            goto done;
        goto closed;
    }
    else {
        device_handle_sync_time_set(dh, NULL);
    }
    retval = 1;
 done:
    if (xt)
        xml_free(xt);
    if (xerr)
        xml_free(xerr);
    if (cberr)
        cbuf_free(cberr);
    if (cbret)
        cbuf_free(cbret);
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Check if there is a location=NETCONF in list
 *
 * @param[in]  dh  Clixon client handle
 * @param[in]  xd  XML tree of netconf monitor schema entry
 * @retval     1   OK, location-netconf
 * @retval     0   No location netconf
 * @see ietf-netconf-monitoring@2010-10-04.yang
 */
static int
schema_check_location_netconf(cxobj *xd)
{
    int      retval = 0;
    cxobj   *x;

    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    x = NULL;
    while ((x = xml_child_each(xd, x, CX_ELMNT)) != NULL) {
        if (strcmp("location", xml_name(x)) != 0)
            continue;
        if (xml_body(x) && strcmp("NETCONF", xml_body(x)) == 0)
            break;
    }
    if (x == NULL)
        goto skip;
    retval = 1;
 skip:
    return retval;
}

/*! Translate from RFC 6022 schemalist to RFC8525 yang-library
 */
static int
schema_list2yang_library(cxobj  *xschemas,
                         cxobj **xyanglib)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    cxobj *x;
    char  *identifier;
    char  *version;
    char  *format;
    char  *namespace;

    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<yang-library xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\">");
    cprintf(cb, "<module-set>");
    cprintf(cb, "<name>mount</name>");
    x = NULL;
    while ((x = xml_child_each(xschemas, x, CX_ELMNT)) != NULL) {
        if (strcmp(xml_name(x), "schema") != 0)
            continue;
        if ((identifier = xml_find_body(x, "identifier")) == NULL ||
            (version = xml_find_body(x, "version")) == NULL ||
            (namespace = xml_find_body(x, "namespace")) == NULL ||
            (format = xml_find_body(x, "format")) == NULL)

            continue;
        if (strcmp(format, "yang") != 0)
            continue;
        if (schema_check_location_netconf(x) == 0)
            continue;
        cprintf(cb, "<module>");
        cprintf(cb, "<name>%s</name>", identifier);
        cprintf(cb, "<revision>%s</revision>", version);
        cprintf(cb, "<namespace>%s</namespace>", namespace);
        cprintf(cb, "</module>");
    }
    cprintf(cb, "</module-set>");
    cprintf(cb, "</yang-library>");
    /* Need yspec to make YB_MODULE */
    if (clixon_xml_parse_string(cbuf_get(cb), YB_NONE, NULL, xyanglib, NULL) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Receive netconf-state schema list from device using RFC 6022 state
 *
 * @param[in] h          Clixon handle.
 * @param[in] dh         Clixon client handle.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] yspec      Yang top-level spec
 * @param[in] rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in] conn_state Device connection state, should be WRESP
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
static int
device_state_recv_schema_list(device_handle dh,
                              cxobj        *xmsg,
                              char         *rpcname,
                              conn_state_t  conn_state)
{
    int    retval = -1;
    char  *rpcprefix;
    char  *namespace = NULL;
    cxobj *xschemas = NULL;
    cxobj *x;
    cxobj *x1;
    cvec  *nsc = NULL;
    cxobj *xyanglib = NULL;

    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    if (strcmp(rpcname, "rpc-reply") != 0){
        device_close_connection(dh, "Unexpected msg %s in state %s",
                         rpcname, device_state_int2str(conn_state));
        goto closed;
    }
    /* get namespace context from rpc-reply */
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    rpcprefix = xml_prefix(xmsg);
    if ((namespace = xml_nsctx_get(nsc, rpcprefix)) == NULL ||
        strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        device_close_connection(dh, "No appropriate namespace associated with:%s",
                   namespace);
        goto closed;
    }
    /* Difficult to use xpath here since prefixes are not known */
    if ((x = xml_find_type(xmsg, NULL, "data", CX_ELMNT)) != NULL &&
        (x1 = xml_find_type(x, NULL, "netconf-state", CX_ELMNT)) != NULL &&
        (xschemas = xml_find_type(x1, NULL, "schemas", CX_ELMNT)) != NULL)
        ;
    else{
        device_close_connection(dh, "No schemas returned");
        goto closed;
    }

    /* Destructive, actually move subtree from xmsg */
    if (xml_rm(xschemas) < 0)
        goto done;
    /* "Wash" it from other elements: eg. junos may sneak in errors
     * XXX Maybe this can be skipped ? v
     */
    x = NULL;
    while ((x = xml_child_each(xschemas, x, CX_ELMNT)) != NULL) {
        if (strcmp(xml_name(x), "schema") != 0)
            xml_flag_set(x, XML_FLAG_MARK);
#ifdef CONTROLLER_JUNOS_SKIP_METADATA
        else if (strcmp(xml_find_body(x,"identifier"), "junos-configuration-metadata") == 0)
            xml_flag_set(x, XML_FLAG_MARK);
#endif
    }
    if (xml_tree_prune_flags(xschemas, XML_FLAG_MARK, XML_FLAG_MARK) < 0)
        goto done;
    /* Translate to RFC 8525 */
    if (schema_list2yang_library(xschemas, &xyanglib) < 0)
        goto done;
    if (xml_rootchild(xyanglib, 0, &xyanglib) < 0)
        goto done;
    if (device_handle_yang_lib_set(dh, xyanglib) < 0)
        goto done;
    retval = 1;
 done:
    return retval;
 closed:
    retval = 0;
    goto done;
}

/*! Receive RFC 6022 get-schema and write to local yang file
 *
 * @param[in] h          Clixon handle.
 * @param[in] dh         Clixon client handle.
 * @param[in] s          Socket where input arrives. Read from this.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] rpcname    Name of RPC, only "hello" is expected here
 * @param[in] conn_state Device connection state, should be CONNECTING
 * @retval    1          OK
 * @retval    0          Closed
 * @retval   -1          Error
 */
static int
device_state_recv_get_schema(device_handle dh,
                             cxobj        *xmsg,
                             char         *rpcname,
                             conn_state_t  conn_state)
{
    int         retval = -1;
    char       *rpcprefix;
    cvec       *nsc = NULL;
    char       *namespace;
    char       *ystr;
    char       *ydec = NULL;
    char       *modname;
    char       *revision = NULL;
    cbuf       *cb = cbuf_new();
    FILE       *f = NULL;
    size_t      sz;
    yang_stmt  *yspec = NULL;

    clicon_debug(1, "%s", __FUNCTION__);
    //    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    if (strcmp(rpcname, "rpc-reply") != 0){
        device_close_connection(dh, "Unexpected msg %s in state %s",
                         rpcname, device_state_int2str(conn_state));
        goto closed;
    }
    /* get namespace context from rpc-reply */
    if (xml_nsctx_node(xmsg, &nsc) < 0)
        goto done;
    rpcprefix = xml_prefix(xmsg);
    if ((namespace = xml_nsctx_get(nsc, rpcprefix)) == NULL ||
        strcmp(namespace, NETCONF_BASE_NAMESPACE) != 0){
        device_close_connection(dh, "No appropriate namespace associated with:%s",
                   namespace);
        goto closed;
    }

    if ((ystr = xml_find_body(xmsg, "data")) == NULL){
        device_close_connection(dh, "Invalid get-schema, no YANG body");
        goto closed;
    }
    if (xml_chardata_decode(&ydec, "%s", ystr) < 0)
        goto done;
    sz = strlen(ydec) + 1;
    revision = device_handle_schema_rev_get(dh);
    modname = device_handle_schema_name_get(dh);
    /* Write to file */
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "%s/%s", YANG_SCHEMA_MOUNT_DIR, modname);
    if (revision)
        cprintf(cb, "@%s", revision);
    cprintf(cb, ".yang");
    clicon_debug(1, "%s: Write yang to %s", __FUNCTION__, cbuf_get(cb));
    if ((f = fopen(cbuf_get(cb), "w")) == NULL){
        clicon_err(OE_UNIX, errno, "fopen(%s)", cbuf_get(cb));
        goto done;
    }
#if 0 // #ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING // XXX Move to parsing
    if (strncmp(modname, "junos-rpc", strlen("junos-rpc")) == 0){
        char *name;
        yang_stmt  *yspec;
        yang_stmt  *ymod;
        yang_stmt  *ygr;

        if ((yspec = yspec_new()) == NULL)
            goto done;
        name = device_handle_name_get(dh);
        /* XXX Parse just to get revision (better get it from get-schema) */
        if ((ymod = yang_parse_str(ydec, name, yspec)) == NULL)
            goto done;
        if (yang_find(ymod, Y_GROUPING, "command-forwarding") == NULL){
            if ((ygr = ys_new(Y_GROUPING)) == NULL)
                goto done;
            if (yang_argument_set(ygr, "command-forwarding") < 0)
                goto done;
            if (yn_insert(ymod, ygr) < 0)
                goto done;
        }
        else{
            if (fwrite(ydec, 1, sz, f) != sz){
                clicon_err(OE_UNIX, errno, "fwrite");
                goto done;
            }
        }
        yang_print(f, ymod);
        // XXX        ys_free(yspec);
    }
#endif
    if (fwrite(ydec, 1, sz, f) != sz){
        clicon_err(OE_UNIX, errno, "fwrite");
        goto done;
    }
    fflush(f);
    retval = 1;
 done:
    if (yspec)
        ys_free(yspec);
    if (f)
        fclose(f);
    if (cb)
        cbuf_free(cb);
    if (ydec)
        free(ydec);
    return retval;
 closed:
    retval = 0;
    goto done;
}

#ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING
/*! Ugly rewrite of junos YANGs after parsing
 * Add grouping command-forwarding in junos-rpc yangs if not exists
 * tried to make other less intrusive solutions or make a generic way in the
 * original function, but the easiest was just to rewrite the function.
 */
int
yang_lib2yspec_junos_patch(clicon_handle h,
                           cxobj        *yanglib,
                           yang_stmt    *yspec)
{
    int        retval = -1;
    cxobj     *xi;
    char      *name;
    char      *revision;
    cvec      *nsc = NULL;
    cxobj    **vec = NULL;
    size_t     veclen;
    int        i;

    clicon_debug(1, "%s", __FUNCTION__);
    if (xpath_vec(yanglib, nsc, "module-set/module", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xi = vec[i];
        if ((name = xml_find_body(xi, "name")) == NULL)
            continue;
        if ((revision = xml_find_body(xi, "revision")) == NULL)
            continue;
#ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING
        {
            yang_stmt  *ymod;
            yang_stmt  *ygr;

            if ((ymod = yang_parse_module(h, name, revision, yspec, NULL)) == NULL)
                goto fail;
            if (yang_find(ymod, Y_GROUPING, "command-forwarding") == NULL){
                if ((ygr = ys_new(Y_GROUPING)) == NULL)
                    goto done;
                if (yang_argument_set(ygr, "command-forwarding") < 0)
                    goto done;
                if (yn_insert(ymod, ygr) < 0)
                    goto done;
            }
        }
#else
        if (yang_parse_module(h, name, revision, yspec, NULL) == NULL)
            goto fail;
#endif
    }
    /* XXX: Ensure yang-lib is always there otherwise get state dont work for mountpoint */
    if (yang_parse_module(h, "ietf-yang-library", "2019-01-04", yspec, NULL) < 0)
        goto fail;
    clicon_debug(1, "%s yang_parse_post", __FUNCTION__);
    if (yang_parse_post(h, yspec, 0) < 0)
        goto done;
    retval = 1;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (vec)
        free(vec);
    return retval;
 fail:
    retval = 0;
    goto done;
}
#endif

/*! All schemas ready from one device, parse the locally
 *
 * @param[in] h          Clixon handle.
 * @param[in] dh         Clixon client handle.
 * @retval    1          OK
 * @retval    0          YANG parse error
 * @retval   -1          Error
 */
static int
device_state_schemas_ready(clixon_handle h,
                           device_handle dh,
                           yang_stmt    *yspec0)
{
    int        retval = -1;
    yang_stmt *yspec1;
    cxobj     *yanglib;
    cxobj     *xt = NULL;
    cxobj     *xmount;
    char      *devname;
    int        ret;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((yspec1 = device_handle_yspec_get(dh)) == NULL){
        clicon_err(OE_YANG, 0, "No yang spec");
        goto done;
    }
    yanglib = device_handle_yang_lib_get(dh);
    /* Given yang-lib, parse all modules into yspec */
#ifdef CONTROLLER_JUNOS_ADD_COMMAND_FORWARDING
    /* Added extra JUNOS patch to mod YANGs */
    if ((ret = yang_lib2yspec_junos_patch(h, yanglib, yspec1)) < 0)
        goto done;
#else
    if ((ret = yang_lib2yspec(h, yanglib, yspec1)) < 0)
        goto done;
#endif
    if (ret == 0)
        goto fail;
    devname = device_handle_name_get(dh);
    /* Create XML tree and device mount-point */
    if (device_state_mount_point_get(devname, yspec0, &xt, &xmount) < 0)
        goto done;
    if (xml_yang_mount_set(xmount, yspec1) < 0)
        goto done;    
    if (ret == 0)
        goto fail;
    yspec1 = NULL;
    retval = 1;
 done:
    clicon_debug(1, "%s retval %d", __FUNCTION__, retval);
    if (yspec1)
        ys_free(yspec1);
    if (xt)
        xml_free(xt);
    return retval;
 fail:
    retval = 0;
    goto done;
}

#ifdef NOTUSED
/*! Controller input wresp to open state handling, read rpc-reply
 *
 * @param[in] dh         Clixon client handle.
 * @param[in] xmsg       XML tree of incoming message
 * @param[in] yspec      Yang top-level spec
 * @param[in] rpcname    Name of RPC, only "rpc-reply" expected here
 * @param[in] conn_state Device connection state, should be WRESP
 * @retval    0          OK
 * @retval   -1          Error
 * @note This is currently not in use, maybe obsolete?
 */
static int
device_state_wresp2open(device_handle dh,
                        cxobj        *xmsg,
                        yang_stmt    *yspec,
                        char         *rpcname,
                        conn_state_t  conn_state)
{
    int           retval = -1;

    if (strcmp(rpcname, "rpc-reply") != 0){
        device_close_connection(dh, "Unexpected msg %s in state %s",
                         rpcname, device_state_int2str(conn_state));
    }
    /* Nothing yet */
    retval = 0;
    // done:
    return retval;
}
#endif

/*! Timeout callback of transient states, close connection
 * @param[in] arg    In effect client handle
 */
static int
device_state_timeout(int   s,
                     void *arg)
{
    device_handle dh = (device_handle)arg;

    device_close_connection(dh, "Timeout waiting for remote peer");
    return 0;
}

/*! Set timeout of transient device state
 * @param[in] arg    In effect client handle
 */
int
device_state_timeout_register(device_handle dh)
{
    int            retval = -1;
    struct timeval t;
    struct timeval t1;
    uint32_t       d;
    clixon_handle  h;
    cbuf          *cb = NULL;

    gettimeofday(&t, NULL);
    h = device_handle_handle_get(dh);
    d = clicon_option_int(h, "controller_device_timeout");
    if (d)
        t1.tv_sec = d;
    else
        t1.tv_sec = 60;
    t1.tv_usec = 0;
    timeradd(&t, &t1, &t);
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "Device %s in state %s",
            device_handle_name_get(dh),
            device_state_int2str(device_handle_conn_state_get(dh)));
    if (clixon_event_reg_timeout(t, device_state_timeout, dh,
                                 cbuf_get(cb)) < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Cancel timeout of transiet device state
 * @param[in] arg    In effect client handle
 */
int
device_state_timeout_unregister(device_handle dh)
{
    int retval = -1;

    if (clixon_event_unreg_timeout(device_state_timeout, dh) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}
    
static int
device_state_timeout_restart(device_handle dh)
{
    int retval = -1;

    if (device_state_timeout_unregister(dh) < 0)
        goto done;
    if (device_state_timeout_register(dh) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Handle controller device state machine
 *
 * @param[in]  h       Clixon handle
 * @param[in]  dh      Clixon client handle.
 * @param[in]  s       Socket
 * @param[in]  xmsg    XML tree of incoming message
 * @retval     0       OK
 * @retval    -1       Error
 */
int
device_state_handler(clixon_handle h,
                     device_handle dh,
                     int           s,
                     cxobj        *xmsg)
{
    int            retval = -1;
    char          *rpcname;
    char          *name;
    conn_state_t   conn_state;
    yang_stmt     *yspec0;
    yang_stmt     *yspec1;
    int            nr;
    int            ret;

    rpcname = xml_name(xmsg);
    conn_state = device_handle_conn_state_get(dh);
    name = device_handle_name_get(dh);
    yspec0 = clicon_dbspec_yang(h);
    switch (conn_state){
    case CS_CONNECTING:
        /* Receive hello from device, send hello */
        if ((ret = device_state_recv_hello(h, dh, s, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
        /* Reset YANGs */
        if ((yspec1 = yspec_new()) == NULL)
            goto done;
        device_handle_yspec_set(dh, yspec1);
        if (!device_handle_capabilities_find(dh, "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring")){
            device_close_connection(dh, "No method to get schemas");
            break;
        }
        if ((ret = device_send_get_schema_list(h, dh, s)) < 0)
            goto done;
        device_handle_conn_state_set(dh, CS_SCHEMA_LIST);
        device_state_timeout_restart(dh);
        break;
    case CS_SCHEMA_LIST:
        /* Receive netconf-state schema list from device */
        if ((ret = device_state_recv_schema_list(dh, xmsg, rpcname, conn_state)) < 0)
            goto done;
        nr = 0;
        if ((ret = device_send_get_schema_next(h, dh, s, &nr)) < 0)
            goto done;
        if (ret == 0){ /* None found */
            /* All schemas ready, parse them */
            if ((ret = device_state_schemas_ready(h, dh, yspec0)) < 0)
                goto done;
            if (ret == 0){
                device_close_connection(dh, "YANG parse error");
                break;
            }
            /* Unconditionally sync */
            if (device_send_sync(h, dh, s) < 0)
                goto done;
            device_handle_conn_state_set(dh, CS_DEVICE_SYNC);
            device_state_timeout_restart(dh);
            break;
        }
        device_handle_nr_schemas_set(dh, nr);
        device_handle_conn_state_set(dh, CS_SCHEMA_ONE);
        device_state_timeout_restart(dh);
        break;
    case CS_SCHEMA_ONE:
        /* Receive get-schema and write to local yang file */
        if ((ret = device_state_recv_get_schema(dh, xmsg, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
        /* Check if all schemas are received */
        nr = device_handle_nr_schemas_get(dh);
        if ((ret = device_send_get_schema_next(h, dh, s, &nr)) < 0)
            goto done;
        if (ret == 0){ /* None sent, done */
            /* All schemas ready, parse them */
            if ((ret = device_state_schemas_ready(h, dh, yspec0)) < 0)
                goto done;
            if (ret == 0){
                device_close_connection(dh, "YANG parse error");
                break;
            }
            /* Unconditionally sync */
            if (device_send_sync(h, dh, s) < 0)
                goto done;
            device_handle_conn_state_set(dh, CS_DEVICE_SYNC);
            device_state_timeout_restart(dh);
            break;
        }
        device_handle_nr_schemas_set(dh, nr);
        device_state_timeout_restart(dh);
        clicon_debug(1, "%s: %s(%d) -> %s(%d)",
                     name,
                     device_state_int2str(conn_state), nr-1,
                     device_state_int2str(conn_state), nr);
        break;
    case CS_DEVICE_SYNC:
        /* Receive config data from device and add config to mount-point */
        if ((ret = device_state_recv_config(h, dh, xmsg, yspec0, rpcname, conn_state)) < 0)
            goto done;
        if (ret == 0) /* closed */
            break;
        device_handle_conn_state_set(dh, CS_OPEN);
        device_state_timeout_unregister(dh);
        break;
#ifdef NOTUSED
    case CS_WRESP: /* XXX currently not used */
        /* Receive get and add to candidate, also commit */
        if (device_state_wresp2open(dh, xmsg, yspec0, rpcname, conn_state) < 0)
            goto done;
        device_handle_conn_state_set(dh, CS_OPEN);
        device_state_timeout_unregister(dh);
        break;
#endif
    case CS_CLOSED:
    case CS_OPEN:
    default:
        clicon_debug(1, "%s %s: Unexpected msg %s in state %s",
                     __FUNCTION__, name, rpcname,
                     device_state_int2str(conn_state));
        clicon_debug_xml(2, xmsg, "Message");
        device_close_connection(dh, "Unexpected msg %s in state %s",
                                rpcname, device_state_int2str(conn_state));
        break;
    }
    retval = 0;
 done:
    return retval;
}