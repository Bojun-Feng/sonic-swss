#ifndef __INTFMGR__
#define __INTFMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <map>
#include <string>
#include <set>
#include <chrono>

struct SubIntfInfo
{
    std::string vlanId;
    std::string mtu;
    std::string adminStatus;
    std::string currAdminStatus;
};

typedef std::map<std::string, SubIntfInfo>             SubIntfMap;
typedef std::map<std::string, bool>                    SagIntfMap;

namespace swss {

class IntfMgr : public Orch
{
public:
    IntfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    ProducerStateTable m_appIntfTableProducer, m_appSagTableProducer;
    Table m_cfgIntfTable, m_cfgVlanIntfTable, m_cfgLagIntfTable, m_cfgLoopbackIntfTable, m_cfgSagTable;
    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateVrfTable, m_stateIntfTable, m_appLagTable;

    Table m_neighTable;
    Table m_rehomeStateTable;
    Table m_appIntfTable;
    DBConnector *m_cfgDb;
    struct RehomeRequest
    {
        std::string id, oldVrf, target, phase, ownerEpoch, recoveryVrf, appliedVrf;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::steady_clock::time_point phaseStarted;
        bool quiesced = false;
        bool cancelling = false;
    };
    std::map<std::string, RehomeRequest> m_rehomeRequests;
    uint64_t m_rehomeSequence = 0;
    bool processRehome(const std::string &alias, std::string &target);
    void sendRehome(const std::string &alias, const RehomeRequest &request,
                    const std::string &phase, const std::string &vrf);
    bool resumeRehomeInterface(const std::string &alias);
    bool quiesceRehomeInterface(const std::string &alias);
    void beginRehomeRecovery(const std::string &alias, RehomeRequest &request, const std::string &epoch);
    void setRehomePhase(const std::string &alias, RehomeRequest &request, const std::string &phase, int budget);
    std::string interfaceConfigTable(const std::string &alias) const;
    void reconcileRehomeAddresses(const std::string &alias);
    bool rehomeControlsCleared(const std::string &alias, const std::string &binding);
    bool holdSagPublication(const std::string &alias, std::vector<FieldValueTuple> &fields);

    SubIntfMap m_subIntfList;
    SagIntfMap m_sagIntfList;
    std::set<std::string> m_loopbackIntfList;
    std::set<std::string> m_pendingReplayIntfList;
    std::set<std::string> m_ipv6LinkLocalModeList;
    std::map<std::string, std::set<std::string>> m_intfLLAddresses;
    std::string mySwitchType;

    void setIntfIp(const std::string &alias, const std::string &opCmd, const IpPrefix &ipPrefix);
    bool setIntfVrf(const std::string &alias, const std::string &vrfName);
    void setIntfMac(const std::string &alias, const std::string &macAddr);
    bool setIntfMpls(const std::string &alias, const std::string &mpls);
    void setIntfState(const std::string &alias, bool isUp);
    void setSagFdbEntry(const std::string &op, const std::string &alias, const std::string &mac_str);

    bool doIntfGeneralTask(const std::vector<std::string>& keys, std::vector<FieldValueTuple> data, const std::string& op);
    bool doIntfAddrTask(const std::vector<std::string>& keys, const std::vector<FieldValueTuple>& data, const std::string& op, bool replace = false);
    void doSagTask(const std::vector<std::string>& keys, const std::vector<FieldValueTuple>& data, const std::string& op);
    void doTask(Consumer &consumer);
    void doPortTableTask(const std::string& key, std::vector<FieldValueTuple> data, std::string op);

    bool isIntfStateOk(const std::string &alias);
    bool isIntfCreated(const std::string &alias);
    bool isIntfChangeVrf(const std::string &alias, const std::string &vrfName);
    int getIntfIpCount(const std::string &alias);
    void buildIntfReplayList(void);
    void setWarmReplayDoneState();

    void addLoopbackIntf(const std::string &alias);
    void delLoopbackIntf(const std::string &alias);
    void flushLoopbackIntfs(void);

    std::string getIntfAdminStatus(const std::string &alias);
    std::string getIntfMtu(const std::string &alias);
    void addHostSubIntf(const std::string&intf, const std::string &subIntf, const std::string &vlan);
    std::string setHostSubIntfMtu(const std::string &alias, const std::string &mtu, const std::string &parent_mtu);
    bool setIntfAdminStatus(const std::string &alias, const std::string &admin_status);
    std::string setHostSubIntfAdminStatus(const std::string &alias, const std::string &admin_status, const std::string &parent_admin_status);
    void removeHostSubIntf(const std::string &subIntf);
    void setSubIntfStateOk(const std::string &alias);
    void removeSubIntfState(const std::string &alias);
    void delIpv6LinkLocalNeigh(const std::string &alias);

    bool setIntfProxyArp(const std::string &alias, const std::string &proxy_arp);
    bool setIntfGratArp(const std::string &alias, const std::string &grat_arp);

    void updateSubIntfAdminStatus(const std::string &alias, const std::string &admin);
    void updateSubIntfMtu(const std::string &alias, const std::string &mtu);
    void updateSagMac(const std::string &macAddr);
    bool enableIpv6Flag(const std::string&);
    void replayLLIntfAddresses(const std::string &alias);

    bool m_replayDone {false};
};

}

#endif
