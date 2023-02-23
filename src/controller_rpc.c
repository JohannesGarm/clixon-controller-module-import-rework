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
  * Backend rpc callbacks, see clixon-controller.yang for declarations
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
#include "controller.h"
#include "controller_device_state.h"
#include "controller_device_handle.h"
#include "controller_device_send.h"
#include "controller_transaction.h"
#include "controller_rpc.h"

/*! Connect to device via Netconf SSH
 *
 * @param[in]  h  Clixon handle
 * @param[in]  dh Device handle, either NULL or in closed state
 * @param[in]  name Device name
 * @param[in]  user Username for ssh login
 * @param[in]  addr Address for ssh to connect to
 */
static int
connect_netconf_ssh(clixon_handle h,
                    device_handle dh,
                    char         *user,
                    char         *addr)
{
    int   retval = -1;
    cbuf *cb = NULL;
    int   s;

    if (addr == NULL || dh == NULL){
        clicon_err(OE_PLUGIN, EINVAL, "xn, addr or dh is NULL");
        goto done;
    }
    if (device_handle_conn_state_get(dh) != CS_CLOSED){
        clicon_err(OE_PLUGIN, EINVAL, "dh is not closed");
        goto done;
    }
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    if (user)
        cprintf(cb, "%s@", user);
    cprintf(cb, "%s", addr);
    if (device_handle_connect(dh, CLIXON_CLIENT_SSH, cbuf_get(cb)) < 0)
        goto done;
    device_state_timeout_register(dh);
    device_handle_conn_state_set(dh, CS_CONNECTING);
    s = device_handle_socket_get(dh);    
    clicon_option_int_set(h, "netconf-framing", NETCONF_SSH_EOM); /* Always start with EOM */
    if (clixon_event_reg_fd(s, device_input_cb, dh, "netconf socket") < 0)
        goto done;
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Connect to device 
 *
 * Typically called from commit
 * @param[in] h   Clixon handle
 * @param[in] xn  XML of device config
 * @retval    0   OK
 * @retval    -1  Error
 */
int
controller_connect(clixon_handle h,
                   cxobj        *xn)
{
    int           retval = -1;
    char         *name;
    device_handle dh;
    cbuf         *cb = NULL;
    char         *type;
    char         *addr;
    char         *user;
    char         *enablestr;
    char         *yfstr;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if ((name = xml_find_body(xn, "name")) == NULL)
        goto ok;
    if ((enablestr = xml_find_body(xn, "enabled")) == NULL){
        goto ok;
    }
    dh = device_handle_find(h, name); /* can be NULL */
    if (strcmp(enablestr, "false") == 0){
        if ((dh = device_handle_new(h, name)) == NULL)
            goto done;
        device_handle_logmsg_set(dh, strdup("Configured down"));
        goto ok;
    }
    if (dh != NULL &&
        device_handle_conn_state_get(dh) != CS_CLOSED)
        goto ok;
    /* Only handle netconf/ssh */
    if ((type = xml_find_body(xn, "conn-type")) == NULL ||
        strcmp(type, "NETCONF_SSH"))
        goto ok;
    if ((addr = xml_find_body(xn, "addr")) == NULL)
        goto ok;
    user = xml_find_body(xn, "user");
    /* Now dh is either NULL or in closed state and with correct type 
     * First create it if still NULL
     */
    if (dh == NULL &&
        (dh = device_handle_new(h, name)) == NULL)
        goto done;
    if ((yfstr = xml_find_body(xn, "yang-config")) != NULL)
        device_handle_yang_config_set(dh, yfstr); /* Cache yang config */    
    if (connect_netconf_ssh(h, dh, user, addr) < 0) /* match */
        goto done;
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}

/*! Incoming rpc handler to sync from one or several devices
 *
 * 1) get previous device synced xml
 * 2) get current and compute diff with previous
 * 3) construct an edit-config, send it and validate it
 * 4) phase 2 commit
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. , if retval = 0
 * @retval     1       OK
 * @retval     0       Fail, cbret set
 * @retval    -1       Error
 */
static int 
push_device(clixon_handle h,
            device_handle dh,
            cbuf         *cbret)
{
    int        retval = -1;
    cxobj     *x0;
    cxobj     *x0copy = NULL;
    cxobj     *x1;
    cxobj     *x1t = NULL;
    cvec      *nsc = NULL;
    cbuf      *cb = NULL;
    char      *name;
    cxobj    **dvec = NULL;
    int        dlen;
    cxobj    **avec = NULL;
    int        alen;
    cxobj    **chvec0 = NULL;
    cxobj    **chvec1 = NULL;
    int        chlen;
    yang_stmt *yspec;

    /* 1) get previous device synced xml */
    if ((x0 = device_handle_sync_xml_get(dh)) == NULL){
        if (netconf_operation_failed(cbret, "application", "No synced device tree")< 0)
            goto done;
        goto fail;
    }
    /* 2) get current and compute diff with previous */
    if ((cb = cbuf_new()) == NULL){
        clicon_err(OE_UNIX, errno, "cbuf_new");
        goto done;
    }
    name = device_handle_name_get(dh);
    cprintf(cb, "devices/device[name='%s']/root", name);
    if (xmldb_get0(h, "running", YB_MODULE, nsc, cbuf_get(cb), 1, WITHDEFAULTS_EXPLICIT, &x1t, NULL, NULL) < 0)
        goto done;
    if ((x1 = xpath_first(x1t, nsc, "%s", cbuf_get(cb))) == NULL){
        if (netconf_operation_failed(cbret, "application", "Device not configured")< 0)
            goto done;
        goto fail;
    }
    if ((yspec = device_handle_yspec_get(dh)) == NULL){
        if (netconf_operation_failed(cbret, "application", "No YANGs in device")< 0)
            goto done;
        goto fail;
    }
    if (xml_diff(yspec, 
                 x0, x1,
                 &dvec, &dlen,
                 &avec, &alen,
                 &chvec0, &chvec1, &chlen) < 0)
        goto done;
    /* 3) construct an edit-config, send it and validate it */
    if (dlen || alen || chlen){
        if ((x0copy = xml_new("new", NULL, xml_type(x0))) == NULL)
            goto done;
        if (xml_copy(x0, x0copy) < 0)
            goto done;
        if (device_send_edit_config_diff(h, dh,
                                         x0copy, x1, yspec,
                                         dvec, dlen,
                                         avec, alen,
                                         chvec0, chvec1, chlen) < 0)
            goto done;
        device_handle_conn_state_set(dh, CS_PUSH_EDIT);
        if (device_state_timeout_register(dh) < 0)
            goto done;
        /* 4) phase 2 commit (XXX later) */
    }
    retval = 1;
 done:
    if (dvec)
        free(dvec);
    if (avec)
        free(avec);
    if (chvec0)
        free(chvec0);
    if (chvec1)
        free(chvec1);
    if (cb)
        cbuf_free(cb);
    if (x0copy)
        xml_free(x0copy);
    if (x1t)
        xml_free(x1t);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Incoming rpc handler to sync from one or several devices
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. if retval = 0
 * @retval     1       OK
 * @retval     0       Fail, cbret set
 * @retval    -1       Error
 */
static int 
pull_device(clixon_handle h,
            device_handle dh,
            cbuf         *cbret)
{
    int  retval = -1;
    int  s;

    clicon_debug(1, "%s", __FUNCTION__);
    s = device_handle_socket_get(dh);
    if (device_send_sync(h, dh, s) < 0)
        goto done;
    device_state_timeout_register(dh);
    device_handle_conn_state_set(dh, CS_DEVICE_SYNC);
    retval = 1;
 done:
    return retval;
}

/*! Incoming rpc handler to sync from or to one or several devices
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  push    0: pull, 1: push
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_sync(clixon_handle h,
         cxobj        *xe,
         cbuf         *cbret,
         int           push)
{
    int           retval = -1;
    char         *pattern = NULL;
    cxobj        *xret = NULL;
    cxobj        *xn;
    cvec         *nsc = NULL;
    cxobj       **vec = NULL;
    size_t        veclen;
    int           i;
    char         *devname;
    device_handle dh;
    int           ret;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (xmldb_get(h, "running", nsc, "devices", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((devname = xml_find_body(xn, "devname")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        if (device_handle_conn_state_get(dh) != CS_OPEN)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        if (push == 1){
            if ((ret = push_device(h, dh, cbret)) < 0)
                goto done;
        }
        else{
            if ((ret = pull_device(h, dh, cbret)) < 0)
                goto done;
        }
        if (ret == 0)
            goto ok;
    } /* for */
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<ok/>");
    cprintf(cbret, "</rpc-reply>");
 ok:
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    return retval;
}
/*! Read the config of one or several devices
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_sync_pull(clixon_handle h,
              cxobj        *xe,
              cbuf         *cbret,
              void         *arg,  
              void         *regarg)
{
    return rpc_sync(h, xe, cbret, 0);
}

/*! Push the config to one or several devices
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_sync_push(clixon_handle h,
              cxobj        *xe,
              cbuf         *cbret,
              void         *arg,  
              void         *regarg)
{
    return rpc_sync(h, xe, cbret, 1);
}


/*! Get last synced configuration of a single device
 *
 * Note that this could be done by some peek in commit history.
 * Should probably be replaced by a more generic function
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_get_device_sync_config(clixon_handle h,
                           cxobj        *xe,
                           cbuf         *cbret,
                           void         *arg,
                           void         *regarg)
{
    int           retval = -1;
    device_handle dh;
    char         *devname;
    cxobj        *xc;

    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<config xmlns=\"%s\">", CONTROLLER_NAMESPACE);
    if ((devname = xml_find_body(xe, "devname")) != NULL &&
        (dh = device_handle_find(h, devname)) != NULL){
        if ((xc = device_handle_sync_xml_get(dh)) != NULL){
            if (clixon_xml2cbuf(cbret, xc, 0, 0, -1, 0) < 0)
                goto done;
        }
    }
    cprintf(cbret, "</config>");
    cprintf(cbret, "</rpc-reply>");
    retval = 0;
 done:
    return retval;
}

/*! (Re)connect try an enabled device in CLOSED state.
 *
 * If closed due to error it may need to be cleared and reconnected
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_reconnect(clixon_handle h,
              cxobj        *xe,
              cbuf         *cbret,
              void         *arg,  
              void         *regarg)
{
    int           retval = -1;
    char         *pattern = NULL;
    cxobj        *xret = NULL;
    cxobj        *xn;
    cvec         *nsc = NULL;
    cxobj       **vec = NULL;
    size_t        veclen;
    int           i;
    char         *devname;
    device_handle dh;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (xmldb_get(h, "running", nsc, "devices", &xret) < 0)
        goto done;
    if (xpath_vec(xret, nsc, "devices/device", &vec, &veclen) < 0) 
        goto done;
    for (i=0; i<veclen; i++){
        xn = vec[i];
        if ((devname = xml_find_body(xn, "devname")) == NULL)
            continue;
        if ((dh = device_handle_find(h, devname)) == NULL)
            continue;
        if (device_handle_conn_state_get(dh) != CS_CLOSED)
            continue;
        if (pattern != NULL && fnmatch(pattern, devname, 0) != 0)
            continue;
        if (controller_connect(h, xn) < 0)
            goto done;
    } /* for */
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<ok/>");
    cprintf(cbret, "</rpc-reply>");
    retval = 0;
 done:
    if (vec)
        free(vec);
    if (xret)
        xml_free(xret);
    return retval;
}

/*! Create a new transaction and allocazte a new transaction id.";
 *
 * If closed due to error it may need to be cleared and reconnected
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_transaction_new(clixon_handle h,
                    cxobj        *xe,
                    cbuf         *cbret,
                    void         *arg,  
                    void         *regarg)
{
    int                     retval = -1;
    char                   *origin;
    controller_transaction *ct = NULL;

    if (controller_transaction_new(h, &ct) < 0)
        goto done;
    if ((origin = xml_find_body(xe, "origin")) != NULL){
        if ((ct->ct_origin = strdup(origin)) == NULL){
            clicon_err(OE_UNIX, errno, "strdup");
            goto done;
        }
    }
    cprintf(cbret, "<rpc-reply xmlns=\"%s\">", NETCONF_BASE_NAMESPACE);
    cprintf(cbret, "<id xmlns=\"%s\">%" PRIu64"</id>", CONTROLLER_NAMESPACE, ct->ct_id);
    cprintf(cbret, "</rpc-reply>");    
    retval = 0;
 done:
    return retval;
}

/*! Terminate an ongoing transaction with an error condition
 *
 * If closed due to error it may need to be cleared and reconnected
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     Domain specific arg, ec client-entry or FCGX_Request 
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 */
static int 
rpc_transaction_error(clixon_handle h,
                      cxobj        *xe,
                      cbuf         *cbret,
                      void         *arg,  
                      void         *regarg)
{
    int           retval = -1;

    retval = 0;
    // done:
    return retval;
}

/*! Register callback for rpc calls */
int
controller_rpc_init(clicon_handle h)
{
    int retval = -1;
    
    if (rpc_callback_register(h, rpc_sync_pull,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "sync-pull"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_sync_push,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "sync-push"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_reconnect,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "reconnect"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_get_device_sync_config,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "get-device-sync-config"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_transaction_new,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "transaction-new"
                              ) < 0)
        goto done;
    if (rpc_callback_register(h, rpc_transaction_error,
                              NULL, 
                              CONTROLLER_NAMESPACE,
                              "transaction-error"
                              ) < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}