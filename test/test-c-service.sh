#!/usr/bin/env bash
#
# Simple C / non-python service checking shared object create and delete
# Uses util/controller_service.c as C-based server
#
# Check starting of service (startup/disable/init)
#
# see https://github.com/SUNET/snc-services/issues/12
# Assume a testA(1) --> testA(2) and a testB and a non-service 0
# where
# testA(1):  Ax, Ay, ABx, ABy
# testA(2)2: Ay, Az, ABy, ABz
# testB:     ABx, ABy, ABz, Bx
#
# Algoritm: Clear actions
#
# Operations shown below, all others keep:
# +------------------------------------------------+
# |                       0x                       |
# +----------------+---------------+---------------+
# |     A0x        |      A0y      |      A0z      |
# +----------------+---------------+---------------+
# |     Ax         |      Ay       |      Az       |
# |    (delete)    |               |     (add)     |
# +----------------+---------------+---------------+
# |     ABx        |      ABy      |      ABz      |
# +----------------+---------------+---------------+
# |                       Bx                       |
# +------------------------------------------------+
#
# Also, after testA(1) do a pull and a restart to ensure creator attributes are intact
#
# For debug start service daemon externally: ${BINDIR}/clixon_controller_service -f $CFG and
# disable CONTROLLER_ACTION_COMMAND
# May fail if python service runs
#

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

if [ $nr -lt 2 ]; then
    echo "Test requires nr=$nr to be greater than 1"
    if [ "$s" = $0 ]; then exit 0; else return 0; fi
fi

dir=/var/tmp/$0
CFG=$dir/controller.xml
CFD=$dir/confdir
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD

fyang=$dir/myyang.yang

: ${clixon_controller_xpath:=clixon_controller_xpath}

# source IMG/USER etc
. ./site.sh

cat<<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$CFG</CLICON_CONFIGFILE>
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_CONFIG_EXTEND>clixon-controller-config</CLICON_CONFIG_EXTEND>
  <CLICON_YANG_DIR>${YANG_INSTALLDIR}</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>$dir</CLICON_YANG_MAIN_DIR>
  <CLICON_CLI_MODE>operation</CLICON_CLI_MODE>
  <CLICON_CLI_DIR>${LIBDIR}/controller/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>${LIBDIR}/controller/clispec</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>${LIBDIR}/controller/backend</CLICON_BACKEND_DIR>
  <CLICON_SOCK>${LOCALSTATEDIR}/run/controller.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>${LOCALSTATEDIR}/run/controller.pid</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>$dir</CLICON_XMLDB_DIR>
  <CLICON_STARTUP_MODE>init</CLICON_STARTUP_MODE>
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_RESTCONF_USER>${CLICON_USER}</CLICON_RESTCONF_USER>
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <CLICON_RESTCONF_INSTALLDIR>${SBINDIR}</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_BACKEND_USER>${CLICON_USER}</CLICON_BACKEND_USER>
  <CLICON_SOCK_GROUP>${CLICON_GROUP}</CLICON_SOCK_GROUP>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>
</clixon-config>
EOF

cat<<EOF > $CFD/action-command.xml
<clixon-config xmlns="http://clicon.org/config">
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_controller_service -f $CFG</CONTROLLER_ACTION_COMMAND>
</clixon-config>
EOF

cat<<EOF > $CFD/autocli.xml
<clixon-config xmlns="http://clicon.org/config">
  <autocli>
     <module-default>false</module-default>
     <list-keyword-default>kw-nokey</list-keyword-default>
     <treeref-state-default>true</treeref-state-default>
     <grouping-treeref>true</grouping-treeref>
     <rule>
       <name>include controller</name>
       <module-name>clixon-controller</module-name>
       <operation>enable</operation>
     </rule>
     <rule>
       <name>include openconfig</name>
       <module-name>openconfig*</module-name>
       <operation>enable</operation>
     </rule>
     <!-- there are many more arista/openconfig top-level modules -->
  </autocli>
</clixon-config>
EOF

cat <<EOF > $fyang
module myyang {
    yang-version 1.1;
    namespace "urn:example:test";
    prefix test;
    import clixon-controller {
      prefix ctrl;
    }
    revision 2023-03-22{
	description "Initial prototype";
    }
    augment "/ctrl:services" {
	list testA {
	    key name;
	    leaf name {
		description "Not used";
		type string;
	    }
	    description "Test service A";
	    leaf-list params{
	       type string;
               min-elements 1; /* For validate fail*/
	   } 
           uses ctrl:created-by-service;
	}
    }
    augment "/ctrl:services" {
	list testB {
	   key name;
	   leaf name {
	      type string;
	   }
	   description "Test service B";
	   leaf-list params{
	      type string;
	   }
           uses ctrl:created-by-service;
	}
    }
}
EOF

# Send process-control and check status of services daemon 
# Args:
# 0: stopped/running   Expected process status
function check_services()
{
    status=$1
    new "Query process-control of action process"
    ret=$(${clixon_netconf} -0 -f $CFG <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
   <process-control $LIBNS>
      <name>Action process</name>
      <operation>status</operation>
   </process-control>
</rpc>]]>]]>
EOF
      )
    new "Check rpc-error"
    match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        err "<reply>" "$ret"
    fi

    new "Check rpc-error status=$status"
    match=$(echo "$ret" | grep --null -Eo "<status $LIBNS>$status</status>") || true
    if [ -z "$match" ]; then
        err "<status>$status</status>" "$ret"
    fi
}

# Enable services process to check for already running
# if you run a separate debug clixon_controller_service process for debugging, set this to false
cat <<EOF > $dir/startup_db
<config>
  <processes xmlns="http://clicon.org/controller">
    <services>
      <enabled>false</enabled>
    </services>
  </processes>
</config>
EOF

if $BE; then
    new "Kill old backend $CFG"
    sudo clixon_backend -f $CFG -z

    new "Start new backend -s startup -f $CFG"
    start_backend -s startup -f $CFG
fi

new "Wait backend 1"
wait_backend

check_services stopped

# see https://github.com/clicon/clixon-controller/issues/84
new "set small timeout"
expectpart "$(${clixon_cli} -m configure -1f $CFG set devices device-timeout 3)" 0 ""

new "commit local"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit local)" 0 ""

new "edit service"
expectpart "$(${clixon_cli} -m configure -1f $CFG set services testA foo params Ax)" 0 ""

new "commit diff w timeout nr 1"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit diff 2>&1)" 0 "Timeout waiting for action daemon"

new "commit diff w timeout nr 2"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit diff 2>&1)" 0 "Timeout waiting for action daemon"

new "discard"
expectpart "$(${clixon_cli} -m configure -1f $CFG discard)" 0 ""

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z
fi

# Then start from startup which by default should start it
# First disable services process
cat <<EOF > $dir/startup_db
<config>
  <processes xmlns="http://clicon.org/controller">
    <services>
      <enabled>true</enabled>
    </services>
  </processes>
</config>
EOF

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Start new backend -s startup -f $CFG -D $DBG"
    sudo clixon_backend -s startup -f $CFG -D $DBG
fi

new "Wait backend 2"
wait_backend

check_services running

new "Start service process, expect fail (already started)"
expectpart "$(clixon_controller_service -f $CFG -1 -l o)" 255 "services-commit client already registered"

# Reset controller by initiating with clixon/openconfig devices and a pull
. ./reset-controller.sh

DEV0="<config>
         <interfaces xmlns=\"http://openconfig.net/yang/interfaces\" xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">
            <interface>
               <name>0x</name>
               <config>
                  <name>0x</name>
                  <type>ianaift:ethernetCsmacd</type>
               </config>
            </interface>
            <interface>
               <name>A0x</name>
               <config>
                  <name>A0x</name>
                  <type>ianaift:ethernetCsmacd</type>
               </config>
            </interface>
            <interface>
               <name>A0y</name>
               <config>
                  <name>A0y</name>
                  <type>ianaift:ethernetCsmacd</type>
               </config>
            </interface>
            <interface>
               <name>A0z</name>
               <config>
                  <name>A0z</name>
                  <type>ianaift:ethernetCsmacd</type>
               </config>
            </interface>
         </interfaces>
      </config>"

new "edit testA(1)"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
	  <testA xmlns="urn:example:test" nc:operation="replace">
	     <name>foo</name>
	     <params>A0x</params>
	     <params>A0y</params>
	     <params>Ax</params>
	     <params>Ay</params>
	     <params>ABx</params>
	     <params>ABy</params>
	 </testA>
	  <testB xmlns="urn:example:test" nc:operation="replace">
	     <name>foo</name>
	     <params>A0x</params>
	     <params>A0y</params>
	     <params>ABx</params>
	     <params>ABy</params>
	     <params>ABz</params>
	     <params>Bx</params>
	 </testB>
      </services>
      <devices xmlns="http://clicon.org/controller">
         <device>
            <name>${IMG}1</name>
            $DEV0
         </device>
         <device>
            <name>${IMG}2</name>
            $DEV0
         </device>
      </devices>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
      )

#echo "$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

sleep $sleep

new "commit push"
set +e
expectpart "$(${clixon_cli} -m configure -1f $CFG commit push 2>&1)" 0 OK --not-- Error

# see https://github.com/clicon/clixon-controller/issues/70
new "commit diff 1"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit diff 2>&1)" 0 OK --not-- "<interface"

# see https://github.com/clicon/clixon-controller/issues/78
new "local change"
expectpart "$(${clixon_cli} -m configure -1f $CFG set devices device openconfig1 config system config login-banner mylogin-banner)" 0 ""

new "commit diff 2"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit diff 2>&1)" 0 "^+\ *<login-banner>mylogin-banner</login-banner>"

new "commit"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit 2>&1)" 0 ""

new "commit diff 3"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit diff 2>&1)" 0 --not-- "^+\ *<login-banner>mylogin-banner</login-banner>"

CREATORSA="<created><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"A0x\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"A0y\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"ABx\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"ABy\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"Ax\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"Ay\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"A0x\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"A0y\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"ABx\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"ABy\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"Ax\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"Ay\"\]</path></created>"

CREATORSB="<created><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"A0x\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"A0y\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"ABx\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"ABy\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"ABz\"\]</path><path>/devices/device\[name=\"openconfig1\"\]/config/interfaces/interface\[name=\"Bx\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"A0x\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"A0y\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"ABx\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"ABy\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"ABz\"\]</path><path>/devices/device\[name=\"openconfig2\"\]/config/interfaces/interface\[name=\"Bx\"\]</path></created>"

new "Check creator attributes testA"
ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <services xmlns="http://clicon.org/controller">
        <testA xmlns="urn:example:test">
          <name>foo</name>
        </testA>
      </services>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"

match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "$CREATORSA") || true
if [ -z "$match" ]; then
    err "$CREATORSA" "$ret"
fi

new "Check creator attributes testB"
ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <services xmlns="http://clicon.org/controller">
        <testB xmlns="urn:example:test">
          <name>foo</name>
        </testB>
      </services>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"

match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "$CREATORSB") || true
if [ -z "$match" ]; then
    err "$CREATORSA" "$ret"
fi

# Pull and ensure attributes remain
new "Pull replace"
expectpart "$(${clixon_cli} -1f $CFG pull)" 0 ""

new "Check creator attributes after pull"
ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <services xmlns="http://clicon.org/controller">
        <testA xmlns="urn:example:test">
          <name>foo</name>
        </testA>
      </services>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "$CREATORSA") || true
if [ -z "$match" ]; then
    err "$CREATORSA" "$ret"
fi

# Restart backend and ensure attributes remain
if $BE; then
    new "Kill old backend $CFG"
    sudo clixon_backend -f $CFG -z
fi

if $BE; then
    new "Start new backend -s running -f $CFG -D $DBG"
    sudo clixon_backend -s running -f $CFG -D $DBG
fi

new "Wait backend 3"
wait_backend

new "open connections"
expectpart "$(${clixon_cli} -1f $CFG connect open)" 0 ""

new "Verify open devices"
sleep $sleep

imax=5
for i in $(seq 1 $imax); do
    res=$(${clixon_cli} -1f $CFG show devices | grep OPEN | wc -l)
    if [ "$res" = "$nr" ]; then
        break;
    fi
    echo "retry $i after sleep"
    sleep $sleep
done
if [ $i -eq $imax ]; then
    err1 "$nr open devices" "$res"
fi

new "Check creator attributes after restart"
ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <services xmlns="http://clicon.org/controller">
        <testA xmlns="urn:example:test">
          <name>foo</name>
        </testA>
      </services>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "$CREATORSA") || true
if [ -z "$match" ]; then
    err "$CREATORSA" "$ret"
fi

new "edit testA(2)"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
	  <testA xmlns="urn:example:test" nc:operation="replace">
	     <name>foo</name>
	     <params>A0y</params>
	     <params>A0z</params>
	     <params>Ay</params>
	     <params>Az</params>
	     <params>ABy</params>
	     <params>ABz</params>
	 </testA>
      </services>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
)

match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

new "commit diff 4"
# Ax removed, Az added
ret=$(${clixon_cli} -m configure -1f $CFG commit diff 2> /dev/null) 
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo '\+ <name>Az</name>') || true
if [ -z "$match" ]; then
    err "commit diff + Az entry" "$ret"
fi
match=$(echo $ret | grep --null -Eo '\- <name>Ax</name') || true
if [ -z "$match" ]; then
    err "diff - Ax entry" "$ret"
fi

# Delete testA completely
new "delete testA(3)"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
          <testA xmlns="urn:example:test" nc:operation="delete">
            <name>foo</name>
          </testA>
      </services>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
)

#echo "$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

sleep $sleep
new "commit push"
set +e
expectpart "$(${clixon_cli} -m configure -1f $CFG commit push 2>&1)" 0 OK --not-- Error

new "get-config check removed Ax"
NAME=clixon-example1
ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <devices xmlns="http://clicon.org/controller">
	<device>
	  <name>$NAME</name>
	  <config>
	    <table xmlns="urn:example:clixon"/>
	  </config>
	</device>
      </devices>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "<parameter><name>Ax</name></parameter>") || true

if [ -n "$match" ]; then
    err "Ax is removed in $NAME" "$ret"
fi

# Delete testB completely
new "delete testB(4)"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
          <testB xmlns="urn:example:test" nc:operation="delete">
            <name>foo</name>
          </testB>
      </services>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
)

#echo "$ret"
match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

sleep $sleep
new "commit push"
set +e
expectpart "$(${clixon_cli} -m configure -1f $CFG commit push 2>&1)" 0 OK --not-- Error

new "get-config check removed Ax"
NAME=clixon-example1
ret=$(${clixon_netconf} -qe0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
message-id="42">
  <get-config>
    <source><running/></source>
    <filter type='subtree'>
      <devices xmlns="http://clicon.org/controller">
	<device>
	  <name>$NAME</name>
	  <config>
	    <table xmlns="urn:example:clixon"/>
	  </config>
	</device>
      </devices>
    </filter>
  </get-config>
</rpc>]]>]]>
EOF
       )
#echo "ret:$ret"
match=$(echo $ret | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

match=$(echo $ret | grep --null -Eo "<parameter><name>Bx</name></parameter>") || true
if [ -n "$match" ]; then
    err "Bx is removed in $NAME" "$ret"
fi


# Edit service, pull, check service still in candidate,
# see https://github.com/clicon/clixon-controller/issues/82
new "edit testC"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
	  <testA xmlns="urn:example:test" nc:operation="replace">
	     <name>fie</name>
	     <params>ZZ</params>
	 </testA>
      </services>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
)

match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

new "show compare service"
expectpart "$($clixon_cli -1 -f $CFG -m configure show compare xml)" 0 "^+\ *<services xmlns=\"http://clicon.org/controller\">" "^+\ *<testA xmlns=\"urn:example:test\">" "^+\ *<name>fie</name>" "^+\ *<params>ZZ</params>"

new "discard"
expectpart "$(${clixon_cli} -1f $CFG -m configure discard)" 0 ""

new "Pull replace"
expectpart "$(${clixon_cli} -1f $CFG pull)" 0 ""

new "Rollback hostnames on openconfig*"
expectpart "$($clixon_cli -1 -f $CFG -m configure rollback)" 0 ""

# Negative errors
new "Create empty testA"
ret=$(${clixon_cli} -m configure -1f $CFG set services testA foo 2> /dev/null) 

new "commit push expect fail"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit push 2>&1)" 255 too-few-elements

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

# Edit sub-service fields https://github.com/clicon/clixon-controller/issues/89
if $BE; then
    new "Start new backend -s running -f $CFG -D $DBG"
    sudo clixon_backend -s running -f $CFG -D $DBG
fi

new "Wait backend 4"
wait_backend

new "open connections"
expectpart "$(${clixon_cli} -1f $CFG connect open)" 0 ""

new "Verify open devices"
sleep $sleep

imax=5
for i in $(seq 1 $imax); do
    res=$(${clixon_cli} -1f $CFG show devices | grep OPEN | wc -l)
    if [ "$res" = "$nr" ]; then
        break;
    fi
    echo "retry $i after sleep"
    sleep $sleep
done
if [ $i -eq $imax ]; then
    err1 "$nr open devices" "$res"
fi

new "Add Sx"
expectpart "$(${clixon_cli} -m configure -1f $CFG set services testA foo params Sx)" 0 "^$"

new "Add Sy"
expectpart "$(${clixon_cli} -m configure -1f $CFG set services testA foo params Sy)" 0 "^$"

new "commit base"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit 2>&1)" 0 "OK"

new "Check Sy"
expectpart "$(${clixon_cli} -1f $CFG show configuration devices device openconfig1 config interfaces interface Sy)" 0 "<name>Sy</name>"

new "remove Sy"
expectpart "$(${clixon_cli} -m configure -1f $CFG delete services testA foo params Sy)" 0 "^$"

new "commit remove"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit 2>&1)" 0 "OK"

new "Check not Sy"
expectpart "$(${clixon_cli} -1f $CFG show configuration devices device openconfig1 config interfaces interface Sy)" 0 --not-- "<name>Sy</name>"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

# apply services
if $BE; then
    new "Start new backend -s running -f $CFG -D $DBG"
    sudo clixon_backend -s running -f $CFG -D $DBG
fi

new "Wait backend 5"
wait_backend

new "open connections"
expectpart "$(${clixon_cli} -1f $CFG connect open)" 0 ""
sleep $sleep

new "edit testA(1)"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
	  <testA xmlns="urn:example:test" nc:operation="replace">
	     <name>foo</name>
	     <params>A0x</params>
	 </testA>
	  <testB xmlns="urn:example:test" nc:operation="replace">
	     <name>foo</name>
	     <params>A0x</params>
	 </testB>
      </services>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
      )

match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

new "commit"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit 2>&1)" 0 "OK"

# XXX: These are not properly tested
new "apply single services diff"
expectpart "$(${clixon_cli} -m configure -1f $CFG apply services "testA[name='foo'] diff" 2>&1)" 0 "OK"

new "apply single services"
expectpart "$(${clixon_cli} -m configure -1f $CFG apply services "testA[name='foo']" 2>&1)" 0 "OK"

new "apply all services"
expectpart "$(${clixon_cli} -m configure -1f $CFG apply services 2>&1)" 0 "OK"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

# Simulated error
cat<<EOF > $CFD/action-command.xml
<clixon-config xmlns="http://clicon.org/config">
  <CONTROLLER_ACTION_COMMAND xmlns="http://clicon.org/controller-config">${BINDIR}/clixon_controller_service -f $CFG -e </CONTROLLER_ACTION_COMMAND>
</clixon-config>
EOF

if $BE; then
    new "Start new backend -s running -f $CFG -D $DBG"
    sudo clixon_backend -s running -f $CFG -D $DBG
fi

# Simulated error
new "Wait backend 6"
wait_backend

new "open connections"
expectpart "$(${clixon_cli} -1f $CFG connect open)" 0 ""

new "Verify open devices"
sleep $sleep

imax=5
for i in $(seq 1 $imax); do
    res=$(${clixon_cli} -1f $CFG show devices | grep OPEN | wc -l)
    if [ "$res" = "$nr" ]; then
        break;
    fi
    echo "retry $i after sleep"
    sleep $sleep
done
if [ $i -eq $imax ]; then
    err1 "$nr open devices" "$res"
fi

new "edit testA(2)"
ret=$(${clixon_netconf} -0 -f $CFG <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
   <capabilities>
      <capability>urn:ietf:params:netconf:base:1.0</capability>
   </capabilities>
</hello>]]>]]>
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
     xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
     message-id="42">
  <edit-config>
    <target><candidate/></target>
    <default-operation>none</default-operation>
    <config>
       <services xmlns="http://clicon.org/controller">
	  <testA xmlns="urn:example:test" nc:operation="replace">
	     <name>foo</name>
	     <params>A0y</params>
	     <params>A0z</params>
	     <params>Ay</params>
	     <params>Az</params>
	     <params>ABy</params>
	     <params>ABz</params>
	 </testA>
      </services>
    </config>
  </edit-config>
</rpc>]]>]]>
EOF
)

match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
if [ -n "$match" ]; then
    err "<ok/>" "$ret"
fi

new "commit"
expectpart "$(${clixon_cli} -m configure -1f $CFG commit 2>&1)" 0 "simulated error"

new "commit diff" # not locked
expectpart "$(${clixon_cli} -m configure -1f $CFG commit diff 2>&1)" 0 "simulated error"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest
