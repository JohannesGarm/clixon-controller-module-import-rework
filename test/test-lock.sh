# Controller test script for exclusive lock on device:
# Open direct connection to device and lock
# Controller edit and commit, expect error

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

set -u

dir=/var/tmp/$0
CFG=$dir/controller.xml
CFD=$dir/conf.d
test -d $dir || mkdir -p $dir
test -d $CFD || mkdir -p $CFD
fin=$dir/in

# source IMG/USER etc
. ./site.sh

cat<<EOF > $CFG
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$CFG</CLICON_CONFIGFILE>
  <CLICON_CONFIGDIR>$CFD</CLICON_CONFIGDIR>
  <CLICON_CONFIG_EXTEND>clixon-controller-config</CLICON_CONFIG_EXTEND>
  <CLICON_FEATURE>ietf-netconf:startup</CLICON_FEATURE>
  <CLICON_FEATURE>clixon-restconf:allow-auth-none</CLICON_FEATURE>
  <CLICON_YANG_DIR>${YANG_INSTALLDIR}</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_DIR>${YANG_INSTALLDIR}/controller/main</CLICON_YANG_MAIN_DIR>
  <CLICON_CLI_MODE>operation</CLICON_CLI_MODE>
  <CLICON_CLI_DIR>${LIBDIR}/controller/cli</CLICON_CLI_DIR>
  <CLICON_CLISPEC_DIR>${dir}</CLICON_CLISPEC_DIR>
  <CLICON_BACKEND_DIR>${LIBDIR}/controller/backend</CLICON_BACKEND_DIR>
  <CLICON_SOCK>${LOCALSTATEDIR}/run/controller.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>${LOCALSTATEDIR}/run/controller.pid</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>${LOCALSTATEDIR}/controller</CLICON_XMLDB_DIR>
  <CLICON_STARTUP_MODE>init</CLICON_STARTUP_MODE>
  <CLICON_SOCK_GROUP>${CLICON_GROUP}</CLICON_SOCK_GROUP>
  <CLICON_STREAM_DISCOVERY_RFC5277>true</CLICON_STREAM_DISCOVERY_RFC5277>
  <CLICON_RESTCONF_USER>${CLICON_USER}</CLICON_RESTCONF_USER>
  <CLICON_RESTCONF_PRIVILEGES>drop_perm</CLICON_RESTCONF_PRIVILEGES>
  <CLICON_RESTCONF_INSTALLDIR>${SBINDIR}</CLICON_RESTCONF_INSTALLDIR>
  <CLICON_VALIDATE_STATE_XML>true</CLICON_VALIDATE_STATE_XML>
  <CLICON_CLI_HELPSTRING_TRUNCATE>true</CLICON_CLI_HELPSTRING_TRUNCATE>
  <CLICON_CLI_HELPSTRING_LINES>1</CLICON_CLI_HELPSTRING_LINES>
  <CLICON_YANG_SCHEMA_MOUNT>true</CLICON_YANG_SCHEMA_MOUNT>
</clixon-config>
EOF

cat <<EOF > $CFD/autocli.xml
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
       <name>include example</name>
       <module-name>openconfig-interfaces</module-name>
       <operation>enable</operation>
     </rule>
     <rule>
       <name>include example</name>
       <module-name>openconfig-system</module-name>
       <operation>enable</operation>
     </rule>
  </autocli>
</clixon-config>
EOF


cat<<EOF > $dir/controller_operation.cli
CLICON_MODE="operation";
CLICON_PROMPT="%U@%H> ";
CLICON_PLUGIN="controller_cli";
# Al11: Auto edit mode
# Autocli syntax tree operations
configure("Change to configure mode"), cli_set_mode("configure");
exit("Quit"), cli_quit();
quit("Quit"), cli_quit();
discard("Discard edits (rollback 0)"), discard_changes();
rollback("Discard edits (rollback 0)"), discard_changes();

show("Show a particular state of the system"){
    compare("Compare candidate and running databases"), compare_dbs_rpc("running", "candidate", "xml");
    configuration("Show configuration"), cli_show_auto_mode("running", "xml", true, false);
    devices("Show state of devices")[
                 (<name:string>("device pattern")|
                  <name:string expand_dbvar("running","/clixon-controller:devices/device/name")>("device pattern"))
                 ], cli_show_devices();{
                 check("Check if device is in sync"), check_device_db("text");
                 detail("Show detailed state"), cli_show_devices("detail");
                 }
    state("Show configuration and state"), cli_show_auto_mode("running", "xml", false, true);
}
pull("sync config from one or multiple devices")[
                 (<name:string>("device pattern")|
                  <name:string expand_dbvar("running","/clixon-controller:devices/device/name")>("device pattern"))
                 ], cli_rpc_pull("replace");{
                    replace, cli_rpc_pull("replace");
                    merge, cli_rpc_pull("merge");
}
EOF

cat<<EOF > $dir/controller_configure.cli
CLICON_MODE="configure";
CLICON_PROMPT="%U@%H[%W]# ";
CLICON_PLUGIN="controller_cli";

exit("Change to operation mode"), cli_set_mode("operation");
operation("run operational commands") @operation;

# Auto edit mode
# Autocli syntax tree operations
edit @datamodelmode, cli_auto_edit("basemodel");
up, cli_auto_up("basemodel");
top, cli_auto_top("basemodel");
set @datamodel, cli_auto_set_devs();
merge @datamodel, cli_auto_merge_devs();
delete("Delete a configuration item") {
      @datamodel, cli_auto_del_devs(); 
      all("Delete whole candidate configuration"), delete_all("candidate");
}
quit("Quit"), cli_quit();
show("Show a particular state of the system"), @datamodelshow, cli_show_auto_mode("candidate", "xml", true, false);
commit("Run services, commit and push to devices"), cli_rpc_controller_commit("candidate", "CHANGE", "COMMIT");
EOF

# Reset devices with initial config
. ./reset-devices.sh

if $BE; then
    new "Kill old backend"
    sudo clixon_backend -s init -f $CFG -z

    new "Start new backend -s init -f $CFG"
    start_backend -s init -f $CFG
fi

new "Wait backend"
wait_backend

# Reset controller
. ./reset-controller.sh

# Start a local blocking thread to openconfig1
i=1
for ip in $CONTAINERS; do
    NAME="$IMG$i"
    new "asynchronous lock running $NAME"
    sleep 60 |  cat <(echo "<?xml version=\"1.0\" encoding=\"UTF-8\"?><hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\"><capabilities><capability>urn:ietf:params:netconf:base:1.0</capability></capabilities></hello>]]>]]><rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" message-id=\"42\"><lock><target><candidate/></target></lock></rpc>]]>]]>") -| ssh -l $USER $ip -o StrictHostKeyChecking=no -o PasswordAuthentication=no -s netconf &

    PIDS=($(jobs -l % | cut -c 6- | awk '{print $1}'))
    break
done

sleep 1

new "Configure hostname on openconfig1"
expectpart "$($clixon_cli -1 -f $CFG -m configure set devices device openconfig1 config system config hostname c-change)" 0 ""

new "Commit 1"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit 2>&1)" 0 "lock is already held in state PUSH_LOCK of device openconfig1" --not-- "OK"

kill ${PIDS[0]}                   # kill the while loop above to close STDIN on 1st
wait

jmax=5
for j in $(seq 1 $jmax); do
    new "Verify open devices 1"
    ret=$(${clixon_netconf} -q0 -f $CFG <<EOF
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="43">
   <get cl:content="all" xmlns:cl="http://clicon.org/lib">
      <nc:filter nc:type="xpath" nc:select="co:devices/co:device/co:conn-state" xmlns:co="http://clicon.org/controller"/>
   </get>
</rpc>]]>]]>
EOF
   )
#    echo "ret:$ret"
    match=$(echo "$ret" | grep --null -Eo "<rpc-error>") || true
    if [ -n "$match" ]; then
        err1 "Error: $ret"
    fi
    echo "$ret" | sed 's/OPEN/OPEN\n/g' 
    res=$(echo "$ret" | sed 's/OPEN/OPEN\n/g' | grep "$IMG" | grep -c "OPEN") || true
    if [ "$res" != "$nr" ]; then
        echo "retry after sleep"
        sleep $sleep
        continue
    fi
    break
done # verify open
if [ $j -eq $jmax ]; then
    err "$nr devices open" "$res devices open"
fi

new "Commit 2"
expectpart "$($clixon_cli -1 -f $CFG -m configure commit 2>&1)" 0 "OK"

if $BE; then
    new "Kill old backend"
    stop_backend -f $CFG
fi

endtest
