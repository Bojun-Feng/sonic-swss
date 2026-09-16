#include <string.h>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "intfmgr.h"
#include "exec.h"
#include "shellcmd.h"
#include "macaddress.h"
#include "warm_restart.h"
#include "subscriberstatetable.h"
#include <swss/redisutility.h>
#include "subintf.h"

using namespace std;
using namespace swss;

#define VLAN_PREFIX         "Vlan"
#define LAG_PREFIX          "PortChannel"
#define SUBINTF_LAG_PREFIX  "Po"
#define LOOPBACK_PREFIX     "Loopback"
#define VNET_PREFIX         "Vnet"
#define MTU_INHERITANCE     "0"
#define VRF_PREFIX          "Vrf"
#define VRF_MGMT            "mgmt"

#define LOOPBACK_DEFAULT_MTU_STR "65536"
#define DEFAULT_MTU_STR 9100
extern MacAddress gMacAddress;
extern MacAddress gSagMacAddress;

IntfMgr::IntfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgIntfTable(cfgDb, CFG_INTF_TABLE_NAME),
        m_cfgVlanIntfTable(cfgDb, CFG_VLAN_INTF_TABLE_NAME),
        m_cfgLagIntfTable(cfgDb, CFG_LAG_INTF_TABLE_NAME),
        m_cfgLoopbackIntfTable(cfgDb, CFG_LOOPBACK_INTERFACE_TABLE_NAME),
        m_cfgSagTable(cfgDb, CFG_SAG_TABLE_NAME),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
        m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME),
        m_stateVrfTable(stateDb, STATE_VRF_TABLE_NAME),
        m_stateIntfTable(stateDb, STATE_INTERFACE_TABLE_NAME),
        m_appIntfTableProducer(appDb, APP_INTF_TABLE_NAME),
        m_appSagTableProducer(appDb, APP_SAG_TABLE_NAME),
        m_neighTable(appDb, APP_NEIGH_TABLE_NAME),
        m_appLagTable(appDb, APP_LAG_TABLE_NAME),
        m_rehomeStateTable(stateDb, "INTERFACE_REHOME_TABLE"),
        m_appIntfTable(appDb, APP_INTF_TABLE_NAME),
        m_cfgDb(cfgDb)
{
    auto subscriberStateTable = new swss::SubscriberStateTable(stateDb,
            STATE_PORT_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 100);
    auto stateConsumer = new Consumer(subscriberStateTable, this, STATE_PORT_TABLE_NAME);
    Orch::addExecutor(stateConsumer);

    auto subscriberStateLagTable = new swss::SubscriberStateTable(stateDb,
            STATE_LAG_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 200);
    auto stateLagConsumer = new Consumer(subscriberStateLagTable, this, STATE_LAG_TABLE_NAME);
    Orch::addExecutor(stateLagConsumer);

    auto feedback = new SubscriberStateTable(stateDb, "INTERFACE_REHOME_TABLE");
    Orch::addExecutor(new Consumer(feedback, this, "INTERFACE_REHOME_TABLE"));

    if (!WarmStart::isWarmStart())
    {
        flushLoopbackIntfs();
        WarmStart::setWarmStartState("intfmgrd", WarmStart::WSDISABLED);
    }
    else
    {
        //Build the interface list to be replayed to Kernel
        buildIntfReplayList();
        if (m_pendingReplayIntfList.empty())
        {
            setWarmReplayDoneState();
        }
    }

    string swtype;
    Table cfgDeviceMetaDataTable(cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);
    if(cfgDeviceMetaDataTable.hget("localhost", "switch_type", swtype))
    {
       mySwitchType = swtype;
    }
}

void IntfMgr::setIntfIp(const string &alias, const string &opCmd,
                        const IpPrefix &ipPrefix)
{
    stringstream    cmd;
    string          res;
    string          ipPrefixStr = ipPrefix.to_string();
    string          broadcastIpStr = ipPrefix.getBroadcastIp().to_string();
    int             prefixLen = ipPrefix.getMaskLength();

    if (ipPrefix.isV4())
    {
        (prefixLen < 31) ?
        (cmd << IP_CMD << " address " << shellquote(opCmd) << " " << shellquote(ipPrefixStr) << " broadcast " << shellquote(broadcastIpStr) <<" dev " << shellquote(alias)) :
        (cmd << IP_CMD << " address " << shellquote(opCmd) << " " << shellquote(ipPrefixStr) << " dev " << shellquote(alias));
    }
    else
    {
        string metric = "";
        // Kernel adds connected route with default metric of 256. But the metric is not
        // communicated to frr unless the ip address is added with explicit metric
        // In voq system, We need the static route to the remote neighbor and connected
        // route to have the same metric to enable BGP to choose paths from routes learned
        // via eBGP and iBGP over the internal inband port be part of same ecmp group.
        // For v4 both the metrics (connected and static) are default 0 so we do not need
        // to set the metric explicitly.
        if(mySwitchType == "voq")
        {
           metric = " metric 256";
        }

        (prefixLen < 127) ?
        (cmd << IP_CMD << " -6 address " << shellquote(opCmd) << " " << shellquote(ipPrefixStr) << " broadcast " << shellquote(broadcastIpStr) <<
         " dev " << shellquote(alias) << metric) :
        (cmd << IP_CMD << " -6 address " << shellquote(opCmd) << " " << shellquote(ipPrefixStr) << " dev " << shellquote(alias) << metric);
    }

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        if (!ipPrefix.isV4() && (opCmd == "add" || opCmd == "replace"))
        {
            SWSS_LOG_NOTICE("Failed to assign IPv6 on interface %s with return code %d, trying to enable IPv6 and retry", alias.c_str(), ret);
            if (!enableIpv6Flag(alias))
            {
                SWSS_LOG_ERROR("Failed to enable IPv6 on interface %s", alias.c_str());
                return;
            }
            ret = swss::exec(cmd.str(), res);
        }

        if (ret)
        {
            SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
        }
    }
}

void IntfMgr::setSagFdbEntry(const string &op, const string &alias, const string &mac_str)
{
    stringstream cmd;
    string res;

    if (op != "add" && op != "replace" && op != "del")
    {
        SWSS_LOG_ERROR("Invalid FDB operation '%s' for MAC %s on %s", op.c_str(), mac_str.c_str(), alias.c_str());
        return;
    }

    if (mac_str == gMacAddress.to_string())
    {
        // Don't add or del for global system MAC address
        return;
    }

    if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        /* Ensure the key starts with "Vlan" otherwise ignore */
        int vlan_id;
        try
        {
            vlan_id = stoi(alias.substr(4));
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Invalid Vlan alias format. Not a number after 'Vlan' prefix: %s", alias.c_str());
            return;
        }

        // cmd format: bridge fdb add 00:11:22:33:44:55 dev Bridge vlan 3 permanent
        cmd << "bridge fdb " << op << " " << mac_str << " dev Bridge vlan " << vlan_id << " permanent";

        int ret = swss::exec(cmd.str(), res);
        if (ret)
        {
            SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
        }
    }
}

void IntfMgr::setIntfMac(const string &alias, const string &mac_str)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link set " << alias << " address " << mac_str;

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
}

bool IntfMgr::setIntfVrf(const string &alias, const string &vrfName)
{
    stringstream cmd;
    string res;

    if (!vrfName.empty())
    {
        cmd << IP_CMD << " link set " << shellquote(alias) << " master " << shellquote(vrfName);
    }
    else
    {
        cmd << IP_CMD << " link set " << shellquote(alias) << " nomaster";
    }
    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
    return ret == 0;
}

bool IntfMgr::setIntfMpls(const string &alias, const string& mpls)
{
    stringstream cmd;
    string res;

    if (mpls == "enable")
    {
        cmd << "sysctl -w net.mpls.conf." << alias << ".input=1";
    }
    else if ((mpls == "disable") || mpls.empty())
    {
        cmd << "sysctl -w net.mpls.conf." << alias << ".input=0";
    }
    else
    {
        SWSS_LOG_ERROR("MPLS state is invalid: \"%s\"", mpls.c_str());
        return false;
    }
    int ret = swss::exec(cmd.str(), res);
    // Don't return error unless MPLS is explicitly set
    if (ret && !mpls.empty())
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
    return true;
}

void IntfMgr::setIntfState(const string &alias, bool isUp)
{
    stringstream cmd;
    string res;

    if (isUp)
    {
        cmd << IP_CMD << " link set " << shellquote(alias) << " up";
    }
    else
    {
        cmd << IP_CMD << " link set " << shellquote(alias) << " down";
    }

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
}

void IntfMgr::addLoopbackIntf(const string &alias)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link add " << alias << " mtu " << LOOPBACK_DEFAULT_MTU_STR << " type dummy";
    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
}

void IntfMgr::delLoopbackIntf(const string &alias)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link del " << alias;
    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
}

void IntfMgr::flushLoopbackIntfs()
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link show type dummy | grep -o '" << LOOPBACK_PREFIX << "[^:]*'";

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_DEBUG("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
        return;
    }

    auto aliases = tokenize(res, '\n');
    for (string &alias : aliases)
    {
        SWSS_LOG_NOTICE("Remove loopback device %s", alias.c_str());
        delLoopbackIntf(alias);
    }
}

int IntfMgr::getIntfIpCount(const string &alias)
{
    stringstream cmd;
    string res;

    /* query ip address of the device with master name, it is much faster */
    // ip address show {{intf_name}}
    // $(ip link show {{intf_name}} | grep -o 'master [^\\s]*') ==> [master {{vrf_name}}]
    // | grep inet | grep -v 'inet6 fe80:' | wc -l
    cmd << IP_CMD << " address show " << alias
        << " $(" << IP_CMD << " link show " << alias << " | grep -o 'master [^\\s]*')"
        << " | grep inet | grep -v 'inet6 fe80:' | wc -l";

    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
        return 0;
    }

    return std::stoi(res);
}

void IntfMgr::buildIntfReplayList(void)
{
    vector<string> intfList;

    m_cfgIntfTable.getKeys(intfList);
    std::copy( intfList.begin(), intfList.end(), std::inserter( m_pendingReplayIntfList, m_pendingReplayIntfList.end() ) );

    m_cfgLoopbackIntfTable.getKeys(intfList);
    std::copy( intfList.begin(), intfList.end(), std::inserter( m_pendingReplayIntfList, m_pendingReplayIntfList.end() ) );

    m_cfgVlanIntfTable.getKeys(intfList);
    std::copy( intfList.begin(), intfList.end(), std::inserter( m_pendingReplayIntfList, m_pendingReplayIntfList.end() ) );

    m_cfgLagIntfTable.getKeys(intfList);
    std::copy( intfList.begin(), intfList.end(), std::inserter( m_pendingReplayIntfList, m_pendingReplayIntfList.end() ) );

    SWSS_LOG_INFO("Found %d Total Intfs to be replayed", (int)m_pendingReplayIntfList.size() );
}

void IntfMgr::setWarmReplayDoneState()
{
    m_replayDone = true;
    WarmStart::setWarmStartState("intfmgrd", WarmStart::REPLAYED);
    // There is no operation to be performed for intfmgr reconcillation
    // Hence mark it reconciled right away
    WarmStart::setWarmStartState("intfmgrd", WarmStart::RECONCILED);
}

bool IntfMgr::isIntfCreated(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (m_stateIntfTable.get(alias, temp))
    {
        SWSS_LOG_DEBUG("Intf %s is ready", alias.c_str());
        return true;
    }

    return false;
}

void IntfMgr::sendRehome(const string &alias, const RehomeRequest &request,
                         const string &phase, const string &vrf)
{
    m_appIntfTableProducer.set(alias, {{"vrf_name", vrf}, {"rehome_id", request.id},
        {"rehome_phase", phase}, {"rehome_target", request.target}, {"rehome_epoch", request.ownerEpoch}});
}

string IntfMgr::interfaceConfigTable(const string &alias) const
{
    if (alias.find('.') != string::npos)
        return CFG_VLAN_SUB_INTF_TABLE_NAME;
    if (alias.compare(0, strlen(LAG_PREFIX), LAG_PREFIX) == 0)
        return CFG_LAG_INTF_TABLE_NAME;
    if (alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX) == 0)
        return CFG_VLAN_INTF_TABLE_NAME;
    return CFG_INTF_TABLE_NAME;
}

void IntfMgr::setRehomePhase(const string &alias, RehomeRequest &request, const string &phase, int budget)
{
    request.phase = phase;
    request.phaseStarted = std::chrono::steady_clock::now();
    request.deadline = request.phaseStarted + std::chrono::seconds(budget);
    m_rehomeStateTable.set(alias, {{"manager_phase", phase}, {"phase_budget_seconds", to_string(budget)}});
}

void IntfMgr::beginRehomeRecovery(const string &alias, RehomeRequest &request, const string &epoch)
{
    request.id = to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + to_string(++m_rehomeSequence);
    request.ownerEpoch = epoch;
    request.cancelling = true;
    request.recoveryVrf = request.oldVrf;
    m_rehomeStateTable.hget(alias, "manager_recovery_vrf", request.recoveryVrf);
    m_rehomeStateTable.set(alias, {{"manager_id", request.id}, {"manager_old_vrf", request.oldVrf},
        {"manager_target", request.target}, {"manager_owner_epoch", epoch},
        {"outcome", "recovering"}});
    setRehomePhase(alias, request, "recover", 0);
    // A restart can interrupt after the link resumes but before admission is released.
    if (request.quiesced)
        quiesceRehomeInterface(alias);
    sendRehome(alias, request, "recover", request.recoveryVrf);
}

bool IntfMgr::quiesceRehomeInterface(const string &alias)
{
    string output;
    return swss::exec("sysctl -w net/ipv6/conf/" + shellquote(alias) + "/keep_addr_on_down=1", output) == 0 &&
           swss::exec(string(IP_CMD) + " link set " + shellquote(alias) + " down", output) == 0;
}

void IntfMgr::reconcileRehomeAddresses(const string &alias)
{
    Table config(m_cfgDb, interfaceConfigTable(alias));
    vector<string> keys;
    vector<FieldValueTuple> fields;
    m_appIntfTable.getKeys(keys);
    for (const auto &key : keys)
    {
        if (key.compare(0, alias.size() + 1, alias + ":") == 0 &&
            !config.get(alias + "|" + key.substr(alias.size() + 1), fields))
            doIntfAddrTask({alias, key.substr(alias.size() + 1)}, {}, DEL_COMMAND);
    }
    config.getKeys(keys);
    for (const auto &key : keys)
    {
        if (key.compare(0, alias.size() + 1, alias + "|") == 0 && config.get(key, fields))
            doIntfAddrTask({alias, key.substr(alias.size() + 1)}, fields, SET_COMMAND, true);
    }
}

bool IntfMgr::resumeRehomeInterface(const string &alias)
{
    string table = CFG_PORT_TABLE_NAME;
    string admin = "up";
    if (alias.find('.') != string::npos)
        table = CFG_VLAN_SUB_INTF_TABLE_NAME;
    else if (alias.compare(0, strlen(LAG_PREFIX), LAG_PREFIX) == 0)
        table = CFG_LAG_TABLE_NAME;
    else if (alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX) == 0)
        table = CFG_VLAN_TABLE_NAME;
    if (table == CFG_PORT_TABLE_NAME || table == CFG_LAG_TABLE_NAME)
        admin = "down";
    Table config(m_cfgDb, table);
    config.hget(alias, "admin_status", admin);
    string output;
    return swss::exec(string(IP_CMD) + " link set " + shellquote(alias) +
                      (admin == "down" ? " down" : " up"), output) == 0;
}

bool IntfMgr::rehomeControlsCleared(const string &alias, const string &binding)
{
    // Producer writes are staged; completion requires the materialized row to be clear.
    vector<FieldValueTuple> fields;
    if (!m_appIntfTable.get(alias, fields))
        return false;
    bool acknowledged = false;
    for (const auto &field : fields)
    {
        const auto &name = fvField(field);
        if (name == "vrf_name")
            acknowledged = fvValue(field) == binding;
        else if ((name == "rehome_id" || name == "rehome_phase" ||
                  name == "rehome_target" || name == "rehome_epoch") && !fvValue(field).empty())
            return false;
    }
    return acknowledged;
}

bool IntfMgr::processRehome(const string &alias, string &target)
{
    constexpr int coordinationBudget = 10;
    constexpr int referenceBudget = 5;
    constexpr int targetBudget = 10;
    string epoch;
    m_rehomeStateTable.hget("__owner__", "epoch", epoch);
    auto entry = m_rehomeRequests.find(alias);
    if (entry == m_rehomeRequests.end())
    {
        string rejected, outcome;
        // An empty manager_target records a cancelled request for the default VRF.
        const bool haveRejected = m_rehomeStateTable.hget(alias, "manager_target", rejected);
        m_rehomeStateTable.hget(alias, "outcome", outcome);
        if (outcome == "pending" || outcome == "recovering")
        {
            string previousPhase;
            m_rehomeStateTable.hget(alias, "manager_phase", previousPhase);
            if (previousPhase == "clearing")
            {
                RehomeRequest resumed;
                resumed.phase = "clearing";
                resumed.target = rejected;
                m_rehomeStateTable.hget(alias, "manager_id", resumed.id);
                m_rehomeStateTable.hget(alias, "manager_owner_epoch", resumed.ownerEpoch);
                m_rehomeStateTable.hget(alias, "manager_old_vrf", resumed.oldVrf);
                if (!m_rehomeStateTable.hget(alias, "manager_recovery_vrf", resumed.appliedVrf))
                    m_stateIntfTable.hget(alias, "vrf", resumed.appliedVrf);
                m_rehomeRequests.emplace(alias, resumed);
                return processRehome(alias, target);
            }
            RehomeRequest request;
            request.quiesced = !previousPhase.empty() && previousPhase != "prepare";
            if (!m_rehomeStateTable.hget(alias, "manager_old_vrf", request.oldVrf))
                m_stateIntfTable.hget(alias, "vrf", request.oldVrf);
            request.target = rejected;
            entry = m_rehomeRequests.emplace(alias, request).first;
            beginRehomeRecovery(alias, entry->second, epoch);
            return false;
        }
        if (outcome == "cancelled" && rejected == target)
        {
            m_stateIntfTable.hget(alias, "vrf", target);
            return true;
        }
        if (!isIntfChangeVrf(alias, target))
        {
            if (outcome == "cancelled" && haveRejected && rejected != target)
            {
                SWSS_LOG_NOTICE("VRF rehome %s cancellation of %s retired by applied request %s",
                                alias.c_str(), rejected.c_str(), target.c_str());
                m_rehomeStateTable.del(alias);
            }
            return true;
        }
        string oldVrf;
        m_stateIntfTable.hget(alias, "vrf", oldVrf);
        if (oldVrf.compare(0, strlen(VNET_PREFIX), VNET_PREFIX) == 0 ||
            target.compare(0, strlen(VNET_PREFIX), VNET_PREFIX) == 0)
            return true;
        const subIntf subIf(alias);
        if (!isIntfStateOk(subIf.isValid() ? subIf.parentIntf() : alias) ||
            (!target.empty() && !isIntfStateOk(target)))
            return false;
        RehomeRequest request;
        request.id = to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + to_string(++m_rehomeSequence);
        request.oldVrf = oldVrf;
        request.target = target;
        request.ownerEpoch = epoch;
        entry = m_rehomeRequests.emplace(alias, request).first;
        m_rehomeStateTable.set(alias, {{"manager_id", request.id}, {"manager_target", target},
            {"manager_old_vrf", oldVrf}, {"manager_recovery_vrf", oldVrf},
            {"manager_owner_epoch", epoch}, {"outcome", "pending"}});
        setRehomePhase(alias, entry->second, "prepare", coordinationBudget);
        sendRehome(alias, entry->second, "prepare", oldVrf);
        return false;
    }
    auto &request = entry->second;
    if (request.phase == "clearing")
    {
        // Do not re-quiesce an applied binding while its control cleanup is pending.
        m_appIntfTableProducer.set(alias, {{"vrf_name", request.appliedVrf}, {"rehome_id", ""},
            {"rehome_phase", ""}, {"rehome_target", ""}, {"rehome_epoch", ""}});
        if (!rehomeControlsCleared(alias, request.appliedVrf))
            return false;
        const string appliedVrf = request.appliedVrf;
        const bool superseded = target != request.target;
        m_rehomeStateTable.set(alias, {{"outcome", appliedVrf == request.target ? "succeeded" : "cancelled"},
            {"manager_phase", "done"}, {"manager_applied_vrf", appliedVrf}});
        m_rehomeRequests.erase(entry);
        if (superseded)
            return processRehome(alias, target);
        target = appliedVrf;
        return true;
    }
    if (request.ownerEpoch != epoch)
    {
        beginRehomeRecovery(alias, request, epoch);
        return false;
    }
    string ownerId, ownerPhase, ownerEpoch;
    m_rehomeStateTable.hget(alias, "owner_id", ownerId);
    m_rehomeStateTable.hget(alias, "owner_phase", ownerPhase);
    m_rehomeStateTable.hget(alias, "owner_epoch", ownerEpoch);
    if (ownerId != request.id || ownerEpoch != epoch)
        ownerPhase.clear();

    const auto now = std::chrono::steady_clock::now();
    const bool ready = request.phase == "drain" && ownerPhase == "ready";
    const bool applied = request.phase == "apply" && ownerPhase == "applied";
    if (!request.cancelling && !applied &&
        (target != request.target || (!ready && now >= request.deadline)))
    {
        string references = "unknown";
        string budget;
        m_rehomeStateTable.hget(alias, "phase_budget_seconds", budget);
        m_rehomeStateTable.hget(alias, "ref_count", references);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - request.phaseStarted).count();
        SWSS_LOG_WARN("VRF rehome %s request %s old %s requested %s phase %s elapsed %lld ms budget %s s owner %s refs %s (unattributed); reconciling binding",
                      alias.c_str(), request.id.c_str(), request.oldVrf.c_str(), request.target.c_str(),
                      request.phase.c_str(), static_cast<long long>(elapsed), budget.c_str(), ownerPhase.c_str(), references.c_str());
        beginRehomeRecovery(alias, request, epoch);
        return false;
    }
    if (request.phase == "prepare" && (ownerPhase == "waiting" || ownerPhase == "ready"))
    {
        if (!quiesceRehomeInterface(alias))
            return false;
        request.quiesced = true;
        setRehomePhase(alias, request, "drain", referenceBudget);
    }
    if (request.phase == "drain" && ownerPhase == "ready")
    {
        setRehomePhase(alias, request, "target", targetBudget);
    }
    if (request.phase == "target")
    {
        if (!isIntfStateOk(request.target.empty() ? alias : request.target) ||
            !setIntfVrf(alias, request.target))
            return false;
        request.phase = "apply";
        m_rehomeStateTable.hset(alias, "manager_phase", "apply");
        sendRehome(alias, request, "apply", request.target);
        return false;
    }
    if (request.phase == "recover" && ownerPhase == "fenced")
    {
        if (!quiesceRehomeInterface(alias) || !setIntfVrf(alias, request.recoveryVrf))
            return false;
        request.quiesced = true;
        reconcileRehomeAddresses(alias);
        sendRehome(alias, request, "restore", request.recoveryVrf);
        request.phase = "restoring";
        m_rehomeStateTable.hset(alias, "manager_phase", "restoring");
        return false;
    }
    if ((request.phase == "apply" && ownerPhase == "applied") ||
        (request.phase == "restoring" && ownerPhase == "restored"))
    {
        reconcileRehomeAddresses(alias);
        if (request.quiesced && !resumeRehomeInterface(alias))
            return false;
        // Recovery retains this RIF after acquisition reopens to new references.
        const string binding = request.cancelling ? request.recoveryVrf : request.target;
        m_rehomeStateTable.hset(alias, "manager_recovery_vrf", binding);
        setRehomePhase(alias, request, "release", coordinationBudget);
        sendRehome(alias, request, "release", binding);
        return false;
    }
    if (request.phase != "release" || ownerPhase != "released")
        return false;

    request.appliedVrf = request.cancelling ? request.recoveryVrf : request.target;
    m_stateIntfTable.hset(alias, "vrf", request.appliedVrf);
    if (request.appliedVrf != request.target)
        SWSS_LOG_WARN("VRF rehome %s request %s cancelled; requested %s not applied, restored %s",
                      alias.c_str(), request.id.c_str(), request.target.c_str(), request.appliedVrf.c_str());
    request.phase = "clearing";
    m_rehomeStateTable.hset(alias, "manager_phase", "clearing");
    return processRehome(alias, target);
}

bool IntfMgr::isIntfChangeVrf(const string &alias, const string &vrfName)
{
    vector<FieldValueTuple> temp;

    if (m_stateIntfTable.get(alias, temp))
    {
        for (auto idx : temp)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);
            if (field == "vrf")
            {
                if (value == vrfName)
                    return false;
                else
                    return true;
            }
        }
    }

    return false;
}

void IntfMgr::addHostSubIntf(const string&intf, const string &subIntf, const string &vlan)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD " link add link " << shellquote(intf) << " name " << shellquote(subIntf) << " type vlan id " << shellquote(vlan);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);
}


std::string IntfMgr::getIntfAdminStatus(const string &alias)
{
    Table *portTable;
    string admin = "down";
    if (!alias.compare(0, strlen("Eth"), "Eth"))
    {
        portTable = &m_statePortTable;
    }
    else if (!alias.compare(0, strlen("Po"), "Po"))
    {
        portTable = &m_stateLagTable;
    }
    else
    {
        return admin;
    }

    vector<FieldValueTuple> temp;
    portTable->get(alias, temp);

    for (auto idx : temp)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);
        if (field == "admin_status")
        {
            admin = value;
        }
    }
    return admin;
}

std::string IntfMgr::getIntfMtu(const string &alias)
{
    Table *portTable;
    string mtu = "0";
    if (!alias.compare(0, strlen("Eth"), "Eth"))
    {
        portTable = &m_statePortTable;
    }
    else if (!alias.compare(0, strlen("Po"), "Po"))
    {
        portTable = &m_stateLagTable;
    }
    else
    {
        return mtu;
    }
    vector<FieldValueTuple> temp;
    portTable->get(alias, temp);
    for (auto idx : temp)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);
        if (field == "mtu")
        {
            mtu = value;
        }
    }
    if (mtu.empty())
    {
        mtu = std::to_string(DEFAULT_MTU_STR);
    }
    return mtu;
}

void IntfMgr::updateSubIntfMtu(const string &alias, const string &mtu)
{
    string intf;
    for (auto entry : m_subIntfList)
    {
        intf = entry.first;
        subIntf subIf(intf);
        if (subIf.parentIntf() == alias)
        {
            std::vector<FieldValueTuple> fvVector;

            string subif_config_mtu = m_subIntfList[intf].mtu;
            if (subif_config_mtu == MTU_INHERITANCE || subif_config_mtu.empty())
                subif_config_mtu = std::to_string(DEFAULT_MTU_STR);

            string subintf_mtu = setHostSubIntfMtu(intf, subif_config_mtu, mtu);

            FieldValueTuple fvTuple("mtu", subintf_mtu);
            fvVector.push_back(fvTuple);
            m_appIntfTableProducer.set(intf, fvVector);
        }
    }
}

std::string IntfMgr::setHostSubIntfMtu(const string &alias, const string &mtu, const string &parent_mtu)
{
    stringstream cmd;
    string res;

    string subifMtu = mtu;
    subIntf subIf(alias);

    int pmtu = (uint32_t)stoul(parent_mtu);
    int cmtu = (uint32_t)stoul(mtu);

    if (pmtu < cmtu)
    {
        subifMtu = parent_mtu;
    }
    SWSS_LOG_INFO("subintf %s active mtu: %s", alias.c_str(), subifMtu.c_str());
    cmd << IP_CMD " link set " << shellquote(alias) << " mtu " << shellquote(subifMtu);
    std::string cmd_str = cmd.str();
    int ret = swss::exec(cmd_str, res);

    if (ret && !isIntfStateOk(alias))
    {
        // Can happen when a SET notification on the PORT_TABLE in the State DB
        // followed by a new DEL notification that send by portmgrd
        SWSS_LOG_WARN("Setting mtu to %s netdev failed with cmd:%s, rc:%d, error:%s", alias.c_str(), cmd_str.c_str(), ret, res.c_str());
    }
    else if (ret)
    {
        throw runtime_error(cmd_str + " : " + res);
    }
    return subifMtu;
}

void IntfMgr::updateSubIntfAdminStatus(const string &alias, const string &admin)
{
    string intf;
    for (auto entry : m_subIntfList)
    {
        intf = entry.first;
        subIntf subIf(intf);
        if (subIf.parentIntf() == alias)
        {
            /*  Avoid duplicate interface admin UP event. */
            string curr_admin = m_subIntfList[intf].currAdminStatus;
            if (curr_admin == "up" && curr_admin == admin)
            {
                continue;
            }
            std::vector<FieldValueTuple> fvVector;
            string subintf_admin = setHostSubIntfAdminStatus(intf, m_subIntfList[intf].adminStatus, admin);
            m_subIntfList[intf].currAdminStatus = subintf_admin;
            FieldValueTuple fvTuple("admin_status", subintf_admin);
            fvVector.push_back(fvTuple);
            m_appIntfTableProducer.set(intf, fvVector);
        }
    }
}

bool IntfMgr::setIntfAdminStatus(const string &alias, const string &admin_status)
{
    stringstream cmd;
    string res, cmd_str;

    SWSS_LOG_INFO("intf %s admin_status: %s", alias.c_str(), admin_status.c_str());
    cmd << IP_CMD " link set " << shellquote(alias) << " " << shellquote(admin_status);
    cmd_str = cmd.str();
    int ret = swss::exec(cmd_str, res);
    if (ret && !isIntfStateOk(alias))
    {
        // Can happen when a DEL notification is sent by portmgrd immediately followed by a new SET notification
        SWSS_LOG_WARN("Setting admin_status to %s netdev failed with cmd:%s, rc:%d, error:%s",
                      alias.c_str(), cmd_str.c_str(), ret, res.c_str());
        return false;
    }
    else if (ret)
    {
        throw runtime_error(cmd_str + " : " + res);
    }
    return true;
}

std::string IntfMgr::setHostSubIntfAdminStatus(const string &alias, const string &admin_status, const string &parent_admin_status)
{
    if (parent_admin_status == "up" || admin_status == "down")
    {
        try
        {
            setIntfAdminStatus(alias, admin_status);
            return admin_status;
        }
        catch (const std::runtime_error &e)
        {
            SWSS_LOG_NOTICE("Set Host subinterface %s admin_status set failure %s failure. Runtime error: %s", alias.c_str(), admin_status.c_str(), e.what());
            throw;
        }
    }
    else
    {
        return "down";
    }
}

void IntfMgr::removeHostSubIntf(const string &subIntf)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD " link del " << shellquote(subIntf);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);
}

void IntfMgr::setSubIntfStateOk(const string &alias)
{
    vector<FieldValueTuple> fvTuples = {{"state", "ok"}};

    if (!alias.compare(0, strlen(SUBINTF_LAG_PREFIX), SUBINTF_LAG_PREFIX))
    {
        m_stateLagTable.set(alias, fvTuples);
    }
    else
    {
        // EthernetX using PORT_TABLE
        m_statePortTable.set(alias, fvTuples);
    }
}

void IntfMgr::removeSubIntfState(const string &alias)
{
    if (!alias.compare(0, strlen(SUBINTF_LAG_PREFIX), SUBINTF_LAG_PREFIX))
    {
        m_stateLagTable.del(alias);
    }
    else
    {
        // EthernetX using PORT_TABLE
        m_statePortTable.del(alias);
    }
}

bool IntfMgr::setIntfGratArp(const string &alias, const string &grat_arp)
{
    /*
     * Enable gratuitous ARP by accepting unsolicited ARP replies and untracked neighbor advertisements
     */
    stringstream cmd;
    string res;
    string garp_enabled;
    int rc;

    if (grat_arp == "enabled")
    {
        garp_enabled = "2";
    }
    else if (grat_arp == "disabled")
    {
        garp_enabled = "0";
    }
    else
    {
        SWSS_LOG_ERROR("GARP state is invalid: \"%s\"", grat_arp.c_str());
        return false;
    }

    cmd << ECHO_CMD << " " << garp_enabled << " > /proc/sys/net/ipv4/conf/" << alias << "/arp_accept";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);
    SWSS_LOG_INFO("ARP accept set to \"%s\" on interface \"%s\"",  grat_arp.c_str(), alias.c_str());

    cmd.clear();
    cmd.str(std::string());

    // `accept_untracked_na` is not available in all kernels, so check for it before trying to set it
    cmd << "test -f /proc/sys/net/ipv6/conf/" << alias << "/accept_untracked_na";
    rc = swss::exec(cmd.str(), res);

    if (rc == 0) {
        cmd.clear();
        cmd.str(std::string());
        cmd << ECHO_CMD << " " << garp_enabled << " > /proc/sys/net/ipv6/conf/" << alias << "/accept_untracked_na";
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
        SWSS_LOG_INFO("`accept_untracked_na` set to \"%s\" on interface \"%s\"",  grat_arp.c_str(), alias.c_str());
    }

    return true;
}

bool IntfMgr::setIntfProxyArp(const string &alias, const string &proxy_arp)
{
    stringstream cmd;
    string res;
    string proxy_arp_status;

    if (proxy_arp == "enabled")
    {
        proxy_arp_status = "1";
    }
    else if (proxy_arp == "disabled")
    {
        proxy_arp_status = "0";
    }
    else
    {
        SWSS_LOG_ERROR("Proxy ARP state is invalid: \"%s\"", proxy_arp.c_str());
        return false;
    }

    cmd << ECHO_CMD << " " << proxy_arp_status << " > /proc/sys/net/ipv4/conf/" << alias << "/proxy_arp_pvlan";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    cmd.clear();
    cmd.str(std::string());

    cmd << ECHO_CMD << " " << proxy_arp_status << " > /proc/sys/net/ipv4/conf/" << alias << "/proxy_arp";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    SWSS_LOG_INFO("Proxy ARP set to \"%s\" on interface \"%s\"", proxy_arp.c_str(), alias.c_str());
    return true;
}

bool IntfMgr::isIntfStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        if (m_stateVlanTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Vlan %s is ready", alias.c_str());
            return true;
        }
    }
    else if (!alias.compare(0, strlen(LAG_PREFIX), LAG_PREFIX))
    {
        if (m_stateLagTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Lag %s is ready", alias.c_str());
            return true;
        }
    }
    else if (!alias.compare(0, strlen(VNET_PREFIX), VNET_PREFIX))
    {
        if (m_stateVrfTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Vnet %s is ready", alias.c_str());
            return true;
        }
    }
    else if ((!alias.compare(0, strlen(VRF_PREFIX), VRF_PREFIX)) ||
            (alias == VRF_MGMT))
    {
        if (m_stateVrfTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Vrf %s is ready", alias.c_str());
            return true;
        }
    }
    else if (m_statePortTable.get(alias, temp))
    {
        auto state_opt = swss::fvsGetValue(temp, "state", true);
        if (!state_opt)
        {
            return false;
        }
        SWSS_LOG_DEBUG("Port %s is ready", alias.c_str());
        return true;
    }
    else if (!alias.compare(0, strlen(LOOPBACK_PREFIX), LOOPBACK_PREFIX))
    {
        return true;
    }
    else if (!alias.compare(0, strlen(SUBINTF_LAG_PREFIX), SUBINTF_LAG_PREFIX))
    {
        if (m_stateLagTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Lag %s is ready", alias.c_str());
            return true;
        }
    }

    return false;
}

void IntfMgr::delIpv6LinkLocalNeigh(const string &alias)
{
    vector<string> neighEntries;

    SWSS_LOG_INFO("Deleting ipv6 link local neighbors for %s", alias.c_str());

    m_neighTable.getKeys(neighEntries);
    for (auto neighKey : neighEntries)
    {
        if (!neighKey.compare(0, alias.size(), alias.c_str()))
        {
            vector<string> keys = tokenize(neighKey, ':', 1);
            if (keys.size() == 2)
            {
                IpAddress ipAddress(keys[1]);
                if (ipAddress.getAddrScope() == IpAddress::AddrScope::LINK_SCOPE)
                {
                    stringstream cmd;
                    string res;

                    cmd << IP_CMD << " neigh del dev " << keys[0] << " " << keys[1] ;
                    swss::exec(cmd.str(), res);
                    SWSS_LOG_INFO("Deleted ipv6 link local neighbor - %s", keys[1].c_str());
                }
            }
        }
    }
}

bool IntfMgr::doIntfGeneralTask(const vector<string>& keys,
        vector<FieldValueTuple> data,
        const string& op)
{
    SWSS_LOG_ENTER();

    string alias(keys[0]);
    string vlanId;
    string parentAlias;
    size_t found = alias.find(VLAN_SUB_INTERFACE_SEPARATOR);
    if (found != string::npos)
    {
        subIntf subIf(alias);
        // alias holds the complete sub interface name
        // while parentAlias holds the parent port name
        /*Check if subinterface is valid and sub interface name length is < 15(IFNAMSIZ)*/
        if (!subIf.isValid())
        {
            SWSS_LOG_ERROR("Invalid subnitf: %s", alias.c_str());
            return true;
        }
        parentAlias = subIf.parentIntf();
        int subIntfId = subIf.subIntfIdx();
        /*If long name format, subinterface Id is vlanid */
        if (!subIf.isShortName())
        {
            vlanId = std::to_string(subIntfId);
            FieldValueTuple vlanTuple("vlan", vlanId);
            data.push_back(vlanTuple);
        }
    }
    bool is_lo = !alias.compare(0, strlen(LOOPBACK_PREFIX), LOOPBACK_PREFIX);
    string mac = "";
    string vrf_name = "";
    string mtu = "";
    string adminStatus = "";
    string nat_zone = "";
    string proxy_arp = "";
    string grat_arp = "";
    string mpls = "";
    string ipv6_link_local_mode = "";
    string sag = "";
    string loopback_action = "";

    for (auto idx : data)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);

        if (field == "vnet_name" || field == "vrf_name")
        {
            vrf_name = value;
        }
        else if (field == "mac_addr")
        {
            mac = value;
        }
        else if (field == "admin_status")
        {
            adminStatus = value;
        }
        else if (field == "proxy_arp")
        {
            proxy_arp = value;
        }
        else if (field == "grat_arp")
        {
            grat_arp = value;
        }
        else if (field == "mpls")
        {
            mpls = value;
        }
        else if (field == "nat_zone")
        {
            nat_zone = value;
        }
        else if (field == "ipv6_use_link_local_only")
        {
            ipv6_link_local_mode = value;
        }
        else if (field == "static_anycast_gateway")
        {
            sag = value;
        }
        else if (field == "vlan")
        {
            vlanId = value;
        }
        else if (field == "loopback_action")
        {
            loopback_action = value;
        }
    }

    if (op == SET_COMMAND)
    {
        string oldVrf;
        m_stateIntfTable.hget(alias, "vrf", oldVrf);
        if (m_rehomeRequests.count(alias) ||
            (!is_lo && oldVrf.compare(0, strlen(VNET_PREFIX), VNET_PREFIX) != 0 &&
             vrf_name.compare(0, strlen(VNET_PREFIX), VNET_PREFIX) != 0))
        {
            if (!processRehome(alias, vrf_name))
                return false;
            for (auto &field : data)
                if (fvField(field) == "vrf_name")
                    fvValue(field) = vrf_name;
        }
        if (!isIntfStateOk(parentAlias.empty() ? alias : parentAlias))
        {
            SWSS_LOG_DEBUG("Interface is not ready, skipping %s", alias.c_str());
            return false;
        }

        if (!vrf_name.empty() && !isIntfStateOk(vrf_name))
        {
            SWSS_LOG_DEBUG("VRF is not ready, skipping %s", vrf_name.c_str());
            return false;
        }

        if (isIntfChangeVrf(alias, vrf_name))
        {
            SWSS_LOG_ERROR("%s can not change VRF binding directly, skipping", alias.c_str());
            return true;
        }

        if (is_lo)
        {
            if (m_loopbackIntfList.find(alias) == m_loopbackIntfList.end())
            {
                addLoopbackIntf(alias);
                m_loopbackIntfList.insert(alias);
                SWSS_LOG_INFO("Added %s loopback interface", alias.c_str());
            }

            if (adminStatus.empty())
            {
                adminStatus = "up";
            }
            else if (adminStatus != "up" && adminStatus != "down")
            {
                SWSS_LOG_WARN("Got incorrect value for admin_status as %s for intf %s, defaulting as up", adminStatus.c_str(), alias.c_str());
                adminStatus = "up";
            }

            try
            {
                if (setIntfAdminStatus(alias, adminStatus))
                {
                    FieldValueTuple newAdminFvTuple("admin_status", adminStatus);
                    data.push_back(newAdminFvTuple);
                }
            }
            catch (const std::runtime_error &e)
            {
                SWSS_LOG_WARN("Lo interface ip link set admin status %s failure. Runtime error: %s", adminStatus.c_str(), e.what());
            }
        }
        else
        {
            /* Set nat zone */
            if (!nat_zone.empty())
            {
                FieldValueTuple fvTuple("nat_zone", nat_zone);
                data.push_back(fvTuple);
            }

            /* Set loopback action */
            if (!loopback_action.empty())
            {
                FieldValueTuple fvTuple("loopback_action", loopback_action);
                data.push_back(fvTuple);
            }

            /* Set mpls */
            if (!setIntfMpls(alias, mpls))
            {
                SWSS_LOG_ERROR("Failed to set MPLS to \"%s\" for the \"%s\" interface", mpls.c_str(), alias.c_str());
                return false;
            }
            if (!mpls.empty())
            {
                FieldValueTuple fvTuple("mpls", mpls);
                data.push_back(fvTuple);
            }

            /* Set ipv6 mode */
            if (!ipv6_link_local_mode.empty())
            {
                if ((ipv6_link_local_mode == "enable") && (m_ipv6LinkLocalModeList.find(alias) == m_ipv6LinkLocalModeList.end()))
                {
                    m_ipv6LinkLocalModeList.insert(alias);
                    SWSS_LOG_INFO("Inserted ipv6 link local mode list for %s", alias.c_str());
                }
                else if ((ipv6_link_local_mode == "disable") && (m_ipv6LinkLocalModeList.find(alias) != m_ipv6LinkLocalModeList.end()))
                {
                    m_ipv6LinkLocalModeList.erase(alias);
                    delIpv6LinkLocalNeigh(alias);
                    SWSS_LOG_INFO("Erased ipv6 link local mode list for %s", alias.c_str());
                }
                FieldValueTuple fvTuple("ipv6_use_link_local_only", ipv6_link_local_mode);
                data.push_back(fvTuple);
            }
        }

        if (!parentAlias.empty())
        {
            subIntf subIf(alias);
            if (m_subIntfList.find(alias) == m_subIntfList.end())
            {
                if (vlanId == "0" || vlanId.empty())
                {
                    SWSS_LOG_INFO("Vlan ID not configured for sub interface %s", alias.c_str());
                    return false;
                }
                try
                {
                    addHostSubIntf(parentAlias, alias, vlanId);
                }
                catch (const std::runtime_error &e)
                {
                    SWSS_LOG_NOTICE("Sub interface ip link add failure. Runtime error: %s", e.what());
                    return false;
                }

                m_subIntfList[alias].vlanId = vlanId;
            }

            if (!mtu.empty())
            {
                string subintf_mtu;
                try
                {
                    string parentMtu = getIntfMtu(subIf.parentIntf());
                    subintf_mtu = setHostSubIntfMtu(alias, mtu, parentMtu);
                    FieldValueTuple fvTuple("mtu", mtu);
                    std::remove(data.begin(), data.end(), fvTuple);
                    FieldValueTuple newMtuFvTuple("mtu", subintf_mtu);
                    data.push_back(newMtuFvTuple);
                }
                catch (const std::runtime_error &e)
                {
                    SWSS_LOG_NOTICE("Sub interface ip link set mtu failure. Runtime error: %s", e.what());
                    return false;
                }
                m_subIntfList[alias].mtu = mtu;
            }
            else
            {
                FieldValueTuple fvTuple("mtu", MTU_INHERITANCE);
                data.push_back(fvTuple);
                m_subIntfList[alias].mtu = MTU_INHERITANCE;
            }

            if (adminStatus.empty())
            {
                adminStatus = "up";
                FieldValueTuple fvTuple("admin_status", adminStatus);
                data.push_back(fvTuple);
            }
            try
            {
                string parentAdmin = getIntfAdminStatus(subIf.parentIntf());
                string subintf_admin = setHostSubIntfAdminStatus(alias, adminStatus, parentAdmin);
                m_subIntfList[alias].currAdminStatus = subintf_admin;
                FieldValueTuple fvTuple("admin_status", adminStatus);
                std::remove(data.begin(), data.end(), fvTuple);
                FieldValueTuple newAdminFvTuple("admin_status", subintf_admin);
                data.push_back(newAdminFvTuple);
            }
            catch (const std::runtime_error &e)
            {
                SWSS_LOG_NOTICE("Sub interface ip link set admin status %s failure. Runtime error: %s", adminStatus.c_str(), e.what());
                return false;
            }
            m_subIntfList[alias].adminStatus = adminStatus;

            // set STATE_DB port state
            setSubIntfStateOk(alias);
        }

        if (!vrf_name.empty())
        {
            setIntfVrf(alias, vrf_name);
        }

        /*Set the mac of interface*/
        if (!mac.empty())
        {
            setIntfMac(alias, mac);
        }
        else
        {
            if (!sag.empty())
            {
                // only VLAN interface can set static anycast gateway
                if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
                {
                    string gwmac = "";
                    if (m_cfgSagTable.hget("GLOBAL", "gateway_mac", gwmac))
                    {
                        // before change interface MAC, set interface down and up to regenerate IPv6 LL by MAC
                        if (sag == "true")
                        {
                            m_sagIntfList[alias] = true;

                            setIntfState(alias, false);
                            setIntfMac(alias, gwmac);
                            setIntfState(alias, true);
                            // add this MAC fdb into bridge
                            setSagFdbEntry("replace", alias, gwmac);

                            FieldValueTuple fvTuple("mac_addr", gwmac);
                            data.push_back(fvTuple);
                        }
                        else if (sag == "false")
                        {
                            m_sagIntfList[alias] = false;

                            // del the sag MAC fdb from bridge
                            setSagFdbEntry("del", alias, gSagMacAddress.to_string());

                            setIntfState(alias, false);
                            setIntfMac(alias, gMacAddress.to_string());
                            setIntfState(alias, true);

                            FieldValueTuple fvTuple("mac_addr", MacAddress().to_string());
                            data.push_back(fvTuple);
                        } else {
                            SWSS_LOG_ERROR("invalid SAG config \"%s\", it should be \"true\" or \"false\"", sag.c_str());
                        }
                    }
                }
            }
            else
            {
                FieldValueTuple fvTuple("mac_addr", MacAddress().to_string());
                data.push_back(fvTuple);
            }
        }

        if (!proxy_arp.empty())
        {
            if (!setIntfProxyArp(alias, proxy_arp))
            {
                SWSS_LOG_ERROR("Failed to set proxy ARP to \"%s\" state for the \"%s\" interface", proxy_arp.c_str(), alias.c_str());
                return false;
            }

            if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
            {
                FieldValueTuple fvTuple("proxy_arp", proxy_arp);
                data.push_back(fvTuple);
            }
        }

        if (!grat_arp.empty())
        {
            if (!setIntfGratArp(alias, grat_arp))
            {
                SWSS_LOG_ERROR("Failed to set ARP accept to \"%s\" state for the \"%s\" interface", grat_arp.c_str(), alias.c_str());
                return false;
            }

            if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
            {
                FieldValueTuple fvTuple("grat_arp", grat_arp);
                data.push_back(fvTuple);
            }
        }

        // Producer writes merge, so clear stale controls explicitly.
        for (const auto &field : {"rehome_id", "rehome_phase", "rehome_target", "rehome_epoch"})
            data.emplace_back(field, "");
        m_appIntfTableProducer.set(alias, data);
        m_stateIntfTable.hset(alias, "vrf", vrf_name);
    }
    else if (op == DEL_COMMAND)
    {
        /* make sure all ip addresses associated with interface are removed, otherwise these ip address would
           be set with global vrf and it may cause ip address conflict. */
        if (getIntfIpCount(alias))
        {
            return false;
        }

        setIntfVrf(alias, "");

        if (is_lo)
        {
            delLoopbackIntf(alias);
            m_loopbackIntfList.erase(alias);
        }

        if (!parentAlias.empty())
        {
            removeHostSubIntf(alias);
            m_subIntfList.erase(alias);

            removeSubIntfState(alias);
        }

        if (m_ipv6LinkLocalModeList.find(alias) != m_ipv6LinkLocalModeList.end())
        {
            m_ipv6LinkLocalModeList.erase(alias);
            delIpv6LinkLocalNeigh(alias);
            SWSS_LOG_INFO("Erased ipv6 link local mode list for %s", alias.c_str());
        }

        if (m_sagIntfList.find(alias) != m_sagIntfList.end())
        {
            if (m_sagIntfList[alias] == true)
            {
                // del the sag MAC fdb from bridge
                setSagFdbEntry("del", alias, gSagMacAddress.to_string());

                // recover to global mac address
                setIntfState(alias, false);
                setIntfMac(alias, gMacAddress.to_string());
                setIntfState(alias, true);
            }
            m_sagIntfList.erase(alias);
        }

        // Restore administrative intent before forgetting a transition on DEL.
        string outcome;
        m_rehomeStateTable.hget(alias, "outcome", outcome);
        if (!is_lo && parentAlias.empty() &&
            (m_rehomeRequests.count(alias) || outcome == "pending" ||
             outcome == "recovering"))
        {
            const string tableName = alias.compare(0, strlen(LAG_PREFIX), LAG_PREFIX) == 0 ?
                CFG_LAG_TABLE_NAME : (alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX) == 0 ?
                CFG_VLAN_TABLE_NAME : CFG_PORT_TABLE_NAME);
            Table config(m_cfgDb, tableName);
            vector<FieldValueTuple> fields;
            if (config.get(alias, fields) && !resumeRehomeInterface(alias))
            {
                SWSS_LOG_ERROR("VRF rehome %s could not restore administrative state on deletion",
                               alias.c_str());
            }
        }

        m_appIntfTableProducer.del(alias);
        m_stateIntfTable.del(alias);
        m_rehomeRequests.erase(alias);
        m_rehomeStateTable.del(alias);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
    }

    return true;
}

bool IntfMgr::doIntfAddrTask(const vector<string>& keys,
        const vector<FieldValueTuple>& data,
        const string& op, bool replace)
{
    SWSS_LOG_ENTER();

    string alias(keys[0]);
    IpPrefix ip_prefix(keys[1]);
    string appKey = keys[0] + ":" + keys[1];

    if (op == SET_COMMAND)
    {
        /*
         * Don't proceed if port/LAG/VLAN/subport and intfGeneral is not ready yet.
         * The pending task will be checked periodically and retried.
         */
        if (!isIntfStateOk(alias) || !isIntfCreated(alias))
        {
            SWSS_LOG_DEBUG("Interface is not ready, skipping %s", alias.c_str());
            return false;
        }

        setIntfIp(alias, replace ? "replace" : "add", ip_prefix);

        if (!ip_prefix.isV4() && ip_prefix.getIp().getAddrScope() == IpAddress::AddrScope::LINK_SCOPE)
        {
            m_intfLLAddresses[alias].insert(ip_prefix.to_string());
        }

        std::vector<FieldValueTuple> fvVector;
        FieldValueTuple f("family", ip_prefix.isV4() ? IPV4_NAME : IPV6_NAME);

        // Don't send ipv4 link local config to AppDB and Orchagent
        if ((ip_prefix.isV4() == false) || (ip_prefix.getIp().getAddrScope() != IpAddress::AddrScope::LINK_SCOPE))
        {
            FieldValueTuple s("scope", "global");
            fvVector.push_back(s);
            fvVector.push_back(f);
            m_appIntfTableProducer.set(appKey, fvVector);
            m_stateIntfTable.hset(keys[0] + state_db_key_delimiter + keys[1], "state", "ok");
        }
    }
    else if (op == DEL_COMMAND)
    {
        setIntfIp(alias, "del", ip_prefix);

        if (!ip_prefix.isV4() && ip_prefix.getIp().getAddrScope() == IpAddress::AddrScope::LINK_SCOPE)
        {
            auto it = m_intfLLAddresses.find(alias);
            if (it != m_intfLLAddresses.end())
            {
                it->second.erase(ip_prefix.to_string());
                if (it->second.empty())
                {
                    m_intfLLAddresses.erase(it);
                }
            }
        }

        // Don't send ipv4 link local config to AppDB and Orchagent
        if ((ip_prefix.isV4() == false) || (ip_prefix.getIp().getAddrScope() != IpAddress::AddrScope::LINK_SCOPE))
        {
            m_appIntfTableProducer.del(appKey);
            m_stateIntfTable.del(keys[0] + state_db_key_delimiter + keys[1]);
        }
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
    }

    return true;
}

void IntfMgr::doSagTask(const vector<string>& keys,
        const vector<FieldValueTuple> &data,
        const string& op)
{
    SWSS_LOG_ENTER();

    string mac = "";
    for (auto idx : data)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);

        if (field == "gateway_mac")
        {
            mac = value;
        }
    }

    vector<FieldValueTuple> fvAppSag;
    if (op == SET_COMMAND)
    {
        if (mac.empty())
        {
            SWSS_LOG_ERROR("gateway_mac field is missing in SAG configuration");
            return;
        }
        FieldValueTuple gwmac("gateway_mac", MacAddress(mac).to_string());
        fvAppSag.push_back(gwmac);
        m_appSagTableProducer.set("GLOBAL", fvAppSag);

        updateSagMac(mac);
    }
    else if (op == DEL_COMMAND)
    {
        m_appSagTableProducer.del("GLOBAL");

        // reset mac address for enabled static-anycast-gateway's VLAN interfaces
        updateSagMac(gMacAddress.to_string());
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
    }
}

void IntfMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    if (table_name == "INTERFACE_REHOME_TABLE")
    {
        // Replayed status can be the only notice of CONFIG deletion during downtime.
        auto updates = std::move(consumer.m_toSync);
        consumer.m_toSync.clear();
        for (const auto &update : updates)
        {
            const string alias = kfvKey(update.second);
            string outcome;
            if (alias == "__owner__" ||
                !m_rehomeStateTable.hget(alias, "outcome", outcome) ||
                (outcome != "pending" && outcome != "recovering"))
            {
                continue;
            }
            const auto tableName = interfaceConfigTable(alias);
            auto *requests = dynamic_cast<Consumer *>(getExecutor(tableName));
            if (!requests)
            {
                continue;
            }
            Table config(m_cfgDb, tableName);
            vector<FieldValueTuple> fields;
            if (!config.get(alias, fields) && !m_rehomeRequests.count(alias))
            {
                requests->addToSync(KeyOpFieldsValuesTuple(alias, DEL_COMMAND, fields));
            }
            requests->drain();
        }
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        if ((table_name == STATE_PORT_TABLE_NAME) || (table_name == STATE_LAG_TABLE_NAME))
        {
            doPortTableTask(kfvKey(t), kfvFieldsValues(t), kfvOp(t));
        }
        else
        {
            vector<string> keys = tokenize(kfvKey(t), config_db_key_delimiter);
            const vector<FieldValueTuple>& data = kfvFieldsValues(t);
            string op = kfvOp(t);

            if (op == SET_COMMAND && !keys.empty())
            {
                const auto root = consumer.m_toSync.find(keys[0]);
                if (root != consumer.m_toSync.end() && kfvOp(root->second) == DEL_COMMAND)
                {
                    ++it;
                    continue;
                }
            }

            if (keys.size() == 1)
            {
                if((table_name == CFG_VOQ_INBAND_INTERFACE_TABLE_NAME) &&
                        (op == SET_COMMAND))
                {
                    //No further processing needed. Just relay to orchagent
                    m_appIntfTableProducer.set(keys[0], data);
                    m_stateIntfTable.hset(keys[0], "vrf", "");

                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                if (table_name == CFG_SAG_TABLE_NAME)
                {
                    doSagTask(keys, data, op);
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                // Retained SETs merge in the consumer; reread the current CONFIG row.
                vector<FieldValueTuple> current = data;
                if (op == SET_COMMAND)
                {
                    Table config(m_cfgDb, table_name);
                    vector<FieldValueTuple> latest;
                    if (config.get(keys[0], latest))
                        current = std::move(latest);
                }
                if (!doIntfGeneralTask(keys, current, op))
                {
                    it++;
                    continue;
                }
                else
                {
                    //Entry programmed, remove it from pending list if present
                    m_pendingReplayIntfList.erase(keys[0]);
                }
            }
            else if (keys.size() == 2)
            {
                if (!doIntfAddrTask(keys, data, op))
                {
                    it++;
                    continue;
                }
                else
                {
                    //Entry programmed, remove it from pending list if present
                    m_pendingReplayIntfList.erase(keys[0] + config_db_key_delimiter + keys[1] );
                }
            }
            else
            {
                SWSS_LOG_ERROR("Invalid key %s", kfvKey(t).c_str());
            }
        }

        it = consumer.m_toSync.erase(it);
    }

    if (!m_replayDone && WarmStart::isWarmStart() && m_pendingReplayIntfList.empty() )
    {
        setWarmReplayDoneState();
    }
}

void IntfMgr::doPortTableTask(const string& key, vector<FieldValueTuple> data, string op)
{
    if (op == SET_COMMAND)
    {
        for (auto idx : data)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);

            if (field == "admin_status")
            {
                SWSS_LOG_INFO("Port %s Admin %s", key.c_str(), value.c_str());
                updateSubIntfAdminStatus(key, value);

                if (value == "up" && m_intfLLAddresses.count(key) > 0)
                {
                    replayLLIntfAddresses(key);
                }
            }
            else if (field == "mtu")
            {
                SWSS_LOG_INFO("Port %s MTU %s", key.c_str(), value.c_str());
                updateSubIntfMtu(key, value);
            }
        }
    }
}

bool IntfMgr::holdSagPublication(const string &alias, vector<FieldValueTuple> &fields)
{
    // A SAG merge must not replace the binding owned by a live transition.
    string outcome;
    m_rehomeStateTable.hget(alias, "outcome", outcome);
    const auto request = m_rehomeRequests.find(alias);
    if (request == m_rehomeRequests.end() && outcome != "pending" && outcome != "recovering")
    {
        return false;
    }
    string phase;
    bool quiesced = false;
    if (request != m_rehomeRequests.end())
    {
        phase = request->second.phase;
        quiesced = request->second.quiesced;
    }
    else
    {
        m_rehomeStateTable.hget(alias, "manager_phase", phase);
        quiesced = !phase.empty() && phase != "prepare";
    }
    for (auto it = fields.begin(); it != fields.end(); ++it)
    {
        if (fvField(*it) == "vrf_name")
        {
            fields.erase(it);
            break;
        }
    }
    return phase == "drain" || phase == "target" || phase == "apply" ||
           phase == "restoring" || (phase == "recover" && quiesced);
}

void IntfMgr::updateSagMac(const std::string &macAddr)
{
    vector<string> keys;
    m_cfgVlanIntfTable.getKeys(keys);
    for (auto &key: keys)
    {
        vector<string> entryKeys = tokenize(key, config_db_key_delimiter);
        if (key.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
        {
            continue;
        }

        // only process the entry includes the SAG's config
        // e.g. VLAN_INTERFACE|Vlan201
        if (entryKeys.size() != 1)
        {
            continue;
        }

        string value = "";
        if (m_cfgVlanIntfTable.hget(key, "static_anycast_gateway", value))
        {
            if (value == "true")
            {
                SWSS_LOG_NOTICE("set %s mac address to %s", key.c_str(), macAddr.c_str());

                vector<FieldValueTuple> vlanIntFv;

                // Get other fields to set them all together
                m_cfgVlanIntfTable.get(key, vlanIntFv);
                const bool heldDown = holdSagPublication(key, vlanIntFv);

                // enable SAG, set device down and up to regenerate IPv6 LL by MAC
                setIntfState(key, false);
                setIntfMac(key, macAddr);
                if (!heldDown)
                    setIntfState(key, true);

                // remove the previous sag MAC fdb from bridge
                setSagFdbEntry("del", key, gSagMacAddress.to_string());
                // add this new MAC fdb into bridge, the "replace" could cover both "add" and "replace" cases.
                setSagFdbEntry("replace", key, macAddr);

                // keep consistent with default MAC 00:00:00:00:00:00
                string entryMac = MacAddress().to_string();
                if (macAddr != gMacAddress.to_string())
                {
                    entryMac = macAddr;
                }

                FieldValueTuple fvTuple("mac_addr", entryMac);
                vlanIntFv.push_back(fvTuple);
                m_appIntfTableProducer.set(key, vlanIntFv);
            }
        } else {
            SWSS_LOG_INFO("can't get %s in VLAN_INTERFACE table", key.c_str());
        }
    }
    gSagMacAddress = MacAddress(macAddr);
}

bool IntfMgr::enableIpv6Flag(const string &alias)
{
    stringstream cmd;
    string temp_res;
    cmd << "sysctl -w net.ipv6.conf." << shellquote(alias) << ".disable_ipv6=0";
    int ret = swss::exec(cmd.str(), temp_res);
    SWSS_LOG_INFO("disable_ipv6 flag is set to 0 for iface: %s, cmd: %s, ret: %d", alias.c_str(), cmd.str().c_str(), ret);
    return (ret == 0) ? true : false;
}

void IntfMgr::replayLLIntfAddresses(const string &alias)
{
    auto it = m_intfLLAddresses.find(alias);
    if (it == m_intfLLAddresses.end())
    {
        return;
    }

    for (const auto &addr : it->second)
    {
        IpPrefix ipPrefix(addr);
        setIntfIp(alias, "add", ipPrefix);
        SWSS_LOG_INFO("Replayed IPv6 link-local address %s on interface %s after admin up",
            addr.c_str(), alias.c_str());
    }
}
