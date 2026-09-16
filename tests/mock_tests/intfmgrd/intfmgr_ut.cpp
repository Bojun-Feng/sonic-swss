#include "gtest/gtest.h"
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sys/stat.h>
#include "../mock_table.h"
#include "warm_restart.h"
#include "macaddress.h"
#define private public
#include "intfmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;
extern swss::MacAddress gMacAddress;
extern swss::MacAddress gSagMacAddress;

bool Ethernet0IPv6Set = false;
bool FailBridgeFdbCommand = false;
bool FailVrfCommand = false;
bool FailDeleteResumeCommand = false;

int cb(const std::string &cmd, std::string &stdout){
    mockCallArgs.push_back(cmd);
    if (FailDeleteResumeCommand && cmd == "/sbin/ip link set \"lo\" up")
        return 1;
    if (FailVrfCommand && cmd.find("/sbin/ip link set \"Ethernet0\" master") == 0)
        return 1;
    if (cmd == "sysctl -w net.ipv6.conf.\"Ethernet0\".disable_ipv6=0") Ethernet0IPv6Set = true;
    else if (cmd.find("/sbin/ip -6 address \"add\"") == 0 ||
             cmd.find("/sbin/ip -6 address \"replace\"") == 0) {
        return Ethernet0IPv6Set ? 0 : 2;
    }
    else if (cmd == "/sbin/ip link set \"Ethernet64.10\" \"up\""){
        return 1;
    }
    else if (cmd.find("/sbin/ip address show ") == 0) {
        stdout = "0\n";
        return 0;
    }
    else if (cmd.find("bridge fdb") == 0) {
        return FailBridgeFdbCommand ? 1 : 0;
    }
    else {
        return 0;
    }
    return 0;
}

// Test Fixture
namespace intfmgr_ut
{
    struct IntfMgrTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;
        std::vector<std::string> cfg_intf_tables;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);

            swss::WarmStart::initialize("intfmgrd", "swss");

            std::vector<std::string> tables = {
                CFG_INTF_TABLE_NAME,
                CFG_LAG_INTF_TABLE_NAME,
                CFG_VLAN_INTF_TABLE_NAME,
                CFG_LOOPBACK_INTERFACE_TABLE_NAME,
                CFG_VLAN_SUB_INTF_TABLE_NAME,
                CFG_VOQ_INBAND_INTERFACE_TABLE_NAME,
            };
            cfg_intf_tables = tables;
            mockCallArgs.clear();
            callback = cb;
            FailBridgeFdbCommand = false;
            FailVrfCommand = false;
            FailDeleteResumeCommand = false;
        }
    };

    static bool commandWasIssued(const std::string &needle)
    {
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    static bool getFieldValue(const std::vector<swss::FieldValueTuple> &values,
                              const std::string &field,
                              std::string &value)
    {
        for (const auto &fv : values)
        {
            if (fvField(fv) == field)
            {
                value = fvValue(fv);
                return true;
            }
        }
        return false;
    }



    struct TestManager : swss::IntfMgr
    {
        using IntfMgr::IntfMgr;
        using Orch::getExecutor;
    };

    TEST_F(IntfMgrTest, RehomeWaitsForParentAndTargetReadiness)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", ""}});
        std::string target = "VrfLate";
        EXPECT_FALSE(mgr.processRehome("Ethernet0", target));
        EXPECT_TRUE(mgr.m_rehomeRequests.empty());
        EXPECT_FALSE(commandWasIssued(" down"));
        mgr.m_stateVrfTable.set(target, {{"state", "ok"}});
        EXPECT_FALSE(mgr.processRehome("Ethernet0", target));
        ASSERT_EQ(mgr.m_rehomeRequests.count("Ethernet0"), 1u);
        EXPECT_EQ(mgr.m_rehomeRequests.at("Ethernet0").phase, "prepare");

        for (const std::string alias : {"Eth64.10", "Po1.20"})
        {
            SCOPED_TRACE(alias);
            mgr.m_stateIntfTable.set(alias, {{"vrf", "VrfOld"}});
            std::string subTarget = "VrfNew";
            mgr.m_stateVrfTable.set(subTarget, {{"state", "ok"}});
            EXPECT_FALSE(mgr.processRehome(alias, subTarget));
            EXPECT_EQ(mgr.m_rehomeRequests.count(alias), 0u);
            if (alias == "Eth64.10")
                mgr.m_statePortTable.set("Ethernet64", {{"state", "ok"}});
            else
                mgr.m_stateLagTable.set("PortChannel1", {{"state", "ok"}});
            EXPECT_FALSE(mgr.processRehome(alias, subTarget));
            ASSERT_EQ(mgr.m_rehomeRequests.count(alias), 1u);
            EXPECT_EQ(mgr.m_rehomeRequests.at(alias).target, subTarget);
        }
    }

    TEST_F(IntfMgrTest, PendingRequestFollowsCurrentConfigIntent)
    {
        TestManager mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", "VrfOld"}});
        mgr.m_stateVrfTable.set("VrfNew", {{"state", "ok"}});
        mgr.m_cfgIntfTable.set("Ethernet0", {{"vrf_name", "VrfNew"}});
        auto *consumer = dynamic_cast<Consumer *>(mgr.getExecutor(CFG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync({"Ethernet0", SET_COMMAND, {{"vrf_name", "VrfNew"}}});
        mgr.doTask(*consumer);
        const auto id = mgr.m_rehomeRequests.at("Ethernet0").id;
        mgr.m_rehomeStateTable.set("Ethernet0", {{"owner_id", id}, {"owner_phase", "waiting"}});
        mgr.doTask(*consumer);
        ASSERT_EQ(mgr.m_rehomeRequests.at("Ethernet0").phase, "drain");

        mgr.m_cfgIntfTable.del("Ethernet0");
        mgr.m_cfgIntfTable.set("Ethernet0", {{"NULL", "NULL"}});
        consumer->addToSync({"Ethernet0", SET_COMMAND, {{"mtu", "9100"}}});
        mgr.doTask(*consumer);
        ASSERT_EQ(mgr.m_rehomeRequests.at("Ethernet0").phase, "recover");
        EXPECT_TRUE(mgr.m_rehomeRequests.at("Ethernet0").cancelling);
        const auto recoveryId = mgr.m_rehomeRequests.at("Ethernet0").id;
        for (const auto &phase : {"fenced", "restored", "released"})
        {
            mgr.m_rehomeStateTable.set("Ethernet0", {{"owner_id", recoveryId}, {"owner_phase", phase}});
            mgr.doTask(*consumer);
        }
        ASSERT_EQ(mgr.m_rehomeRequests.count("Ethernet0"), 1u);
        EXPECT_EQ(mgr.m_rehomeRequests.at("Ethernet0").target, "");
        EXPECT_EQ(mgr.m_rehomeRequests.at("Ethernet0").phase, "prepare");
    }

    TEST_F(IntfMgrTest, VnetBindingIsOutOfScopeAndSupersedesTheAttempt)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        mgr.m_stateVrfTable.set("VrfRed", {{"state", "ok"}});
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", ""}});
        std::string target = "VrfRed";
        EXPECT_FALSE(mgr.processRehome("Ethernet0", target));
        target = "VnetRed";
        EXPECT_FALSE(mgr.processRehome("Ethernet0", target));
        EXPECT_EQ(mgr.m_rehomeRequests.at("Ethernet0").phase, "recover");
        const auto id = mgr.m_rehomeRequests.at("Ethernet0").id;
        for (const auto &phase : {"fenced", "restored"})
        {
            mgr.m_rehomeStateTable.set("Ethernet0", {{"owner_id", id}, {"owner_phase", phase}});
            EXPECT_FALSE(mgr.processRehome("Ethernet0", target));
        }
        mgr.m_rehomeStateTable.hset("Ethernet0", "owner_phase", "released");
        EXPECT_TRUE(mgr.processRehome("Ethernet0", target));
        EXPECT_TRUE(mgr.m_rehomeRequests.empty());
        EXPECT_TRUE(mgr.processRehome("Ethernet0", target));
        EXPECT_TRUE(mgr.m_rehomeRequests.empty());
        EXPECT_FALSE(commandWasIssued("master \"VnetRed\""));
    }

    TEST_F(IntfMgrTest, CancellationRestoresTheOldBindingAndIsNotRetried)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", ""}});
        mgr.m_stateVrfTable.set("VrfRed", {{"state", "ok"}});
        const std::vector<swss::FieldValueTuple> data = {{"vrf_name", "VrfRed"}};
        EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        auto &request = mgr.m_rehomeRequests.at("Ethernet0");
        const auto id = request.id;
        EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(
                      request.deadline - request.phaseStarted).count(), 10);

        mgr.m_rehomeStateTable.set("Ethernet0", {{"owner_id", "stale"}, {"owner_phase", "ready"}});
        EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        EXPECT_FALSE(commandWasIssued("\"Ethernet0\" down"));

        mgr.m_rehomeStateTable.set("Ethernet0", {{"owner_id", id}, {"owner_phase", "waiting"}});
        EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        EXPECT_TRUE(commandWasIssued("\"Ethernet0\" down"));
        EXPECT_EQ(request.phase, "drain");
        EXPECT_EQ(std::chrono::duration_cast<std::chrono::seconds>(
                      request.deadline - request.phaseStarted).count(), 5);

        request.deadline = std::chrono::steady_clock::now();
        EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        const auto recoveryId = request.id;
        EXPECT_NE(recoveryId, id);
        std::string value;
        mgr.m_rehomeStateTable.hget("Ethernet0", "outcome", value);
        EXPECT_EQ(value, "recovering");
        EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        mgr.m_rehomeStateTable.set("Ethernet0", {{"owner_id", recoveryId}, {"owner_phase", "fenced"}});
        EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        mgr.m_rehomeStateTable.hset("Ethernet0", "owner_phase", "restored");
        EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        mgr.m_rehomeStateTable.hget("Ethernet0", "outcome", value);
        EXPECT_EQ(value, "recovering");
        mgr.m_rehomeStateTable.hset("Ethernet0", "owner_phase", "released");
        EXPECT_TRUE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        mgr.m_rehomeStateTable.hget("Ethernet0", "outcome", value);
        EXPECT_EQ(value, "cancelled");

        EXPECT_TRUE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        EXPECT_TRUE(mgr.m_rehomeRequests.empty());
        EXPECT_FALSE(commandWasIssued("master \"VrfRed\""));
        mgr.m_stateVrfTable.set("VrfBlue", {{"state", "ok"}});
        EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, {{"vrf_name", "VrfBlue"}}, SET_COMMAND));
        EXPECT_NE(mgr.m_rehomeRequests.at("Ethernet0").id, id);
    }

    TEST_F(IntfMgrTest, CancelledTargetIsRearmedByAProcessedAppliedBinding)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        for (const std::string applied : {"VrfBlue", ""})
        {
            SCOPED_TRACE(applied.empty() ? "default" : applied);
            mgr.m_rehomeStateTable.del("Ethernet0");
            mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", applied}});
            mgr.m_stateVrfTable.set("VrfRed", {{"state", "ok"}});
            mgr.m_rehomeStateTable.set("Ethernet0", {{"outcome", "cancelled"},
                {"manager_phase", "done"}, {"manager_target", "VrfRed"},
                {"manager_applied_vrf", applied}});
            std::string target = "VrfRed";
            EXPECT_TRUE(mgr.processRehome("Ethernet0", target));
            EXPECT_EQ(target, applied);
            EXPECT_TRUE(mgr.m_rehomeRequests.empty());
            std::string outcome;
            EXPECT_TRUE(mgr.m_rehomeStateTable.hget("Ethernet0", "outcome", outcome));

            target = applied;
            EXPECT_TRUE(mgr.processRehome("Ethernet0", target));
            EXPECT_FALSE(mgr.m_rehomeStateTable.hget("Ethernet0", "outcome", outcome));
            target = "VrfRed";
            EXPECT_FALSE(mgr.processRehome("Ethernet0", target));
            ASSERT_EQ(mgr.m_rehomeRequests.count("Ethernet0"), 1u);
            EXPECT_EQ(mgr.m_rehomeRequests.at("Ethernet0").target, "VrfRed");
            mgr.m_rehomeRequests.clear();
        }
    }

    TEST_F(IntfMgrTest, RootDeletionSupersedesTheRequestAndRestoresAdminIntent)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        swss::Table port(m_config_db.get(), CFG_PORT_TABLE_NAME);
        port.set("lo", {{"admin_status", "up"}});
        for (bool restarted : {false, true})
        {
            SCOPED_TRACE(restarted);
            mgr.m_rehomeRequests.clear();
            mgr.m_stateIntfTable.set("lo", {{"vrf", "VrfOld"}});
            mgr.m_rehomeStateTable.set("lo", {{"manager_phase", "drain"}});
            if (restarted)
                mgr.m_rehomeStateTable.hset("lo", "outcome", "pending");
            else
                mgr.m_rehomeRequests["lo"].quiesced = true;
            port.hset("lo", "admin_status", "up");
            mockCallArgs.clear();
            EXPECT_TRUE(mgr.doIntfGeneralTask({"lo"}, {}, DEL_COMMAND));
            EXPECT_TRUE(commandWasIssued("\"lo\" up"));
            EXPECT_FALSE(commandWasIssued("address \"add\""));
            std::string value;
            EXPECT_FALSE(mgr.m_stateIntfTable.hget("lo", "vrf", value));
            EXPECT_FALSE(mgr.m_rehomeStateTable.hget("lo", "manager_phase", value));
            EXPECT_EQ(mgr.m_rehomeRequests.count("lo"), 0u);
        }

        port.hset("lo", "admin_status", "down");
        mgr.m_rehomeStateTable.set("lo", {{"manager_phase", "drain"}, {"outcome", "pending"}});
        mockCallArgs.clear();
        EXPECT_TRUE(mgr.doIntfGeneralTask({"lo"}, {}, DEL_COMMAND));
        EXPECT_TRUE(commandWasIssued("\"lo\" down"));
        EXPECT_FALSE(commandWasIssued("\"lo\" up"));
        for (const auto &alias : {"Ethernet0", "PortChannel1", "Vlan100", "Ethernet0.100"})
        {
            mockCallArgs.clear();
            ASSERT_TRUE(mgr.resumeRehomeInterface(alias));
            const std::string expected = std::string(alias) == "Ethernet0" ||
                std::string(alias) == "PortChannel1" ? " down" : " up";
            EXPECT_TRUE(commandWasIssued(std::string("\"") + alias + "\"" + expected));
        }

        for (const auto &outcome : {"", "cancelled", "succeeded"})
        {
            mgr.m_rehomeStateTable.set("lo", {{"outcome", outcome}});
            mockCallArgs.clear();
            EXPECT_TRUE(mgr.doIntfGeneralTask({"lo"}, {}, DEL_COMMAND));
            EXPECT_FALSE(commandWasIssued("\"lo\" up"));
            EXPECT_FALSE(commandWasIssued("\"lo\" down"));
        }
        port.del("lo");
        mgr.m_rehomeStateTable.set("lo", {{"outcome", "recovering"}});
        mockCallArgs.clear();
        EXPECT_TRUE(mgr.doIntfGeneralTask({"lo"}, {}, DEL_COMMAND));
        EXPECT_FALSE(commandWasIssued("\"lo\" up"));
    }

    TEST_F(IntfMgrTest, RecreateWaitsBehindAPendingRootDeletion)
    {
        TestManager mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), {CFG_INTF_TABLE_NAME});
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", ""}});
        mgr.m_cfgIntfTable.set("Ethernet0", {{"NULL", "NULL"}});
        mgr.m_cfgIntfTable.set("Ethernet0|192.0.2.1/24", {{"NULL", "NULL"}});
        static bool kernelHasAddress;
        kernelHasAddress = true;
        callback = [](const std::string &cmd, std::string &output) {
            if (cmd.find("/sbin/ip address show ") == 0)
            {
                output = kernelHasAddress ? "1\n" : "0\n";
                return 0;
            }
            if (cmd.find("address \"del\"") != std::string::npos) kernelHasAddress = false;
            if (cmd.find("address \"replace\"") != std::string::npos ||
                cmd.find("address \"add\"") != std::string::npos) kernelHasAddress = true;
            return cb(cmd, output);
        };
        auto consumer = dynamic_cast<Consumer *>(mgr.getExecutor(CFG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync(swss::KeyOpFieldsValuesTuple("Ethernet0|192.0.2.1/24", DEL_COMMAND, {}));
        consumer->addToSync(swss::KeyOpFieldsValuesTuple("Ethernet0", DEL_COMMAND, {}));
        consumer->addToSync({"Ethernet0", SET_COMMAND, {{"NULL", "NULL"}}});
        consumer->addToSync({"Ethernet0|192.0.2.1/24", SET_COMMAND, {{"NULL", "NULL"}}});
        mgr.doTask(*consumer);
        EXPECT_FALSE(kernelHasAddress);
        EXPECT_EQ(consumer->m_toSync.count("Ethernet0"), 2u);
        mgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(kernelHasAddress);
        std::string applied;
        EXPECT_TRUE(mgr.m_stateIntfTable.hget("Ethernet0", "vrf", applied));
        EXPECT_TRUE(applied.empty());
        callback = cb;
    }

    TEST_F(IntfMgrTest, AcknowledgementFromARestartedOwnerIsRejected)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        mgr.m_stateVrfTable.set("VrfRed", {{"state", "ok"}});
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", ""}});
        mgr.m_rehomeStateTable.set("__owner__", {{"epoch", "first"}});
        std::string target = "VrfRed";
        EXPECT_FALSE(mgr.processRehome("Ethernet0", target));
        const auto id = mgr.m_rehomeRequests.at("Ethernet0").id;
        mgr.m_rehomeStateTable.set("Ethernet0", {{"owner_id", id}, {"owner_phase", "ready"}});
        mgr.m_rehomeStateTable.hset("__owner__", "epoch", "second");
        EXPECT_FALSE(mgr.processRehome("Ethernet0", target));
        EXPECT_FALSE(commandWasIssued("\"Ethernet0\" down"));
        std::string outcome;
        mgr.m_rehomeStateTable.hget("Ethernet0", "outcome", outcome);
        EXPECT_EQ(outcome, "recovering");
        const auto recoveryId = mgr.m_rehomeRequests.at("Ethernet0").id;
        EXPECT_NE(recoveryId, id);
        mgr.m_rehomeStateTable.set("Ethernet0", {{"owner_id", recoveryId},
            {"owner_epoch", "second"}, {"owner_phase", "fenced"}});
        EXPECT_FALSE(mgr.processRehome("Ethernet0", target));
        EXPECT_EQ(mgr.m_rehomeRequests.at("Ethernet0").phase, "restoring");
    }

    TEST_F(IntfMgrTest, RestartResumesFromDurableIntentAndKeepsTheAcknowledgedBinding)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", "VrfOld"}});
        mgr.m_rehomeStateTable.set("__owner__", {{"epoch", "owner"}});
        mgr.m_rehomeStateTable.set("Ethernet0", {{"outcome", "pending"},
            {"manager_phase", "release"}, {"manager_id", "before-restart"},
            {"manager_target", "VrfRed"}, {"manager_old_vrf", "VrfOld"},
            {"owner_id", "before-restart"}, {"owner_epoch", "owner"}, {"owner_phase", "applied"}});
        std::string target = "VrfNew";
        EXPECT_FALSE(mgr.processRehome("Ethernet0", target));
        const auto &request = mgr.m_rehomeRequests.at("Ethernet0");
        EXPECT_EQ(request.oldVrf, "VrfOld");
        EXPECT_EQ(request.target, "VrfRed");
        EXPECT_NE(request.id, "before-restart");
        EXPECT_EQ(request.phase, "recover");
        EXPECT_TRUE(commandWasIssued("\"Ethernet0\" down"));
        EXPECT_FALSE(mgr.processRehome("Ethernet0", target));
        EXPECT_FALSE(commandWasIssued("\"Ethernet0\" master"));
        EXPECT_FALSE(commandWasIssued("\"Ethernet0\" up"));

        swss::IntfMgr restarted(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        restarted.m_rehomeStateTable.set("Ethernet0", {{"outcome", "pending"},
            {"manager_phase", "release"}, {"manager_id", "previous"},
            {"manager_target", "VrfRed"}, {"manager_old_vrf", "VrfOld"},
            {"manager_recovery_vrf", "VrfRed"}, {"manager_owner_epoch", "owner"},
            {"owner_id", "previous"}, {"owner_epoch", "owner"}, {"owner_phase", "released"}});
        mockCallArgs.clear();
        target = "VrfRed";
        ASSERT_FALSE(restarted.processRehome("Ethernet0", target));
        const auto id = restarted.m_rehomeRequests.at("Ethernet0").id;
        EXPECT_NE(id, "previous");
        EXPECT_EQ(restarted.m_rehomeRequests.at("Ethernet0").recoveryVrf, "VrfRed");
        for (const auto &phase : {"fenced", "restored", "released"})
        {
            restarted.m_rehomeStateTable.set("Ethernet0", {{"owner_id", id},
                {"owner_epoch", "owner"}, {"owner_phase", phase}});
            restarted.processRehome("Ethernet0", target);
        }
        std::string value;
        EXPECT_TRUE(restarted.m_rehomeStateTable.hget("Ethernet0", "outcome", value));
        EXPECT_EQ(value, "succeeded");
        EXPECT_TRUE(commandWasIssued("master \"VrfRed\""));
        EXPECT_FALSE(commandWasIssued("master \"VrfOld\""));
        EXPECT_TRUE(restarted.m_rehomeRequests.empty());
    }

    TEST_F(IntfMgrTest, OwnerFeedbackDrivesTheRequestAndCleansUpAWithdrawnRow)
    {
        TestManager mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", ""}});
        mgr.m_stateVrfTable.set("VrfRed", {{"state", "ok"}});
        mgr.m_cfgIntfTable.set("Ethernet0", {{"vrf_name", "VrfRed"}});
        auto *requests = dynamic_cast<Consumer *>(mgr.getExecutor(CFG_INTF_TABLE_NAME));
        auto *feedback = dynamic_cast<Consumer *>(mgr.getExecutor("INTERFACE_REHOME_TABLE"));
        ASSERT_NE(requests, nullptr);
        ASSERT_NE(feedback, nullptr);
        requests->addToSync({"Ethernet0", SET_COMMAND, {{"vrf_name", "VrfRed"}}});
        mgr.doTask(*requests);
        auto &request = mgr.m_rehomeRequests.at("Ethernet0");
        ASSERT_EQ(request.phase, "prepare");
        ASSERT_EQ(requests->m_toSync.count("Ethernet0"), 1u);

        mgr.m_rehomeStateTable.set("Ethernet0", {{"owner_id", request.id}, {"owner_phase", "waiting"}});
        feedback->addToSync(swss::KeyOpFieldsValuesTuple("Ethernet0", SET_COMMAND, {}));
        mgr.doTask(*feedback);
        EXPECT_TRUE(request.quiesced);
        EXPECT_EQ(request.phase, "drain");

        TestManager afterRestart(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        swss::Table port(m_config_db.get(), CFG_PORT_TABLE_NAME);
        port.set("Ethernet0", {{"admin_status", "up"}});
        afterRestart.m_cfgIntfTable.del("Ethernet0");
        afterRestart.m_rehomeStateTable.set("Ethernet0", {{"outcome", "pending"},
            {"manager_phase", "drain"}, {"manager_id", "before-restart"},
            {"manager_target", "VrfRed"}, {"manager_old_vrf", ""}});
        swss::Table appIntfs(m_app_db.get(), APP_INTF_TABLE_NAME);
        appIntfs.set("Ethernet0:192.0.2.1/24", {{"family", "IPv4"}, {"scope", "global"}});
        static int kernelAddresses;
        kernelAddresses = 1;
        callback = [](const std::string &cmd, std::string &output) {
            if (cmd.find("/sbin/ip address show ") == 0)
            {
                output = std::to_string(kernelAddresses) + "\n";
                return 0;
            }
            if (cmd.find("address \"del\"") != std::string::npos && kernelAddresses > 0)
            {
                --kernelAddresses;
            }
            return cb(cmd, output);
        };
        mockCallArgs.clear();
        auto *replay = dynamic_cast<Consumer *>(afterRestart.getExecutor("INTERFACE_REHOME_TABLE"));
        ASSERT_NE(replay, nullptr);
        replay->addToSync(swss::KeyOpFieldsValuesTuple("__owner__", SET_COMMAND, {}));
        replay->addToSync(swss::KeyOpFieldsValuesTuple("Ethernet0", SET_COMMAND, {}));
        afterRestart.doTask(*replay);

        EXPECT_TRUE(commandWasIssued("address \"del\" \"192.0.2.1/24\""));
        EXPECT_EQ(kernelAddresses, 0);
        std::vector<swss::FieldValueTuple> published;
        EXPECT_FALSE(appIntfs.get("Ethernet0:192.0.2.1/24", published));
        EXPECT_TRUE(commandWasIssued("\"Ethernet0\" up"));
        std::string value;
        EXPECT_FALSE(afterRestart.m_rehomeStateTable.hget("Ethernet0", "outcome", value));
        EXPECT_FALSE(afterRestart.m_stateIntfTable.hget("Ethernet0", "vrf", value));

        auto *afterRequests = dynamic_cast<Consumer *>(afterRestart.getExecutor(CFG_INTF_TABLE_NAME));
        ASSERT_NE(afterRequests, nullptr);
        EXPECT_EQ(afterRequests->m_toSync.count("Ethernet0"), 0u);
        afterRestart.m_cfgIntfTable.set("Ethernet0", {{"NULL", "NULL"}});
        afterRequests->addToSync({"Ethernet0", SET_COMMAND, {{"NULL", "NULL"}}});
        afterRestart.doTask(*afterRequests);
        EXPECT_TRUE(afterRequests->m_toSync.empty());
        EXPECT_TRUE(afterRestart.m_stateIntfTable.hget("Ethernet0", "vrf", value));
        callback = cb;
    }

    TEST_F(IntfMgrTest, KernelFailureInsideTheTargetPhaseIsRetriedThenCancelled)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", "VrfBlue"}});
        mgr.m_stateVrfTable.set("VrfRed", {{"state", "ok"}});
        std::string target = "VrfRed";
        ASSERT_FALSE(mgr.processRehome("Ethernet0", target));
        auto &request = mgr.m_rehomeRequests.at("Ethernet0");
        mgr.m_rehomeStateTable.set("Ethernet0", {{"owner_id", request.id}, {"owner_phase", "ready"}});

        FailVrfCommand = true;
        ASSERT_FALSE(mgr.processRehome("Ethernet0", target));
        EXPECT_EQ(request.phase, "target");
        ASSERT_FALSE(mgr.processRehome("Ethernet0", target));
        EXPECT_EQ(request.phase, "target");
        swss::Table appIntfs(m_app_db.get(), APP_INTF_TABLE_NAME);
        std::string published;
        ASSERT_TRUE(appIntfs.hget("Ethernet0", "rehome_phase", published));
        EXPECT_EQ(published, "prepare");

        request.deadline = std::chrono::steady_clock::now();
        ASSERT_FALSE(mgr.processRehome("Ethernet0", target));
        EXPECT_EQ(request.phase, "recover");
        std::string outcome;
        mgr.m_rehomeStateTable.hget("Ethernet0", "outcome", outcome);
        EXPECT_EQ(outcome, "recovering");
    }

    TEST_F(IntfMgrTest, AddressReconciliationRestoresCurrentConfigIntent)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", ""}});
        for (const auto &address : {"192.0.2.1/24", "2001:db8::1/64"})
            mgr.m_cfgIntfTable.set(std::string("Ethernet0|") + address, {{"NULL", "NULL"}});
        swss::Table appIntfs(m_app_db.get(), APP_INTF_TABLE_NAME);
        appIntfs.set("Ethernet0:198.51.100.1/24", {{"family", "IPv4"}});
        Ethernet0IPv6Set = false;

        mgr.reconcileRehomeAddresses("Ethernet0");

        EXPECT_FALSE(commandWasIssued("address \"add\""));
        EXPECT_TRUE(commandWasIssued("address \"replace\" \"192.0.2.1/24\""));
        EXPECT_TRUE(commandWasIssued("address \"replace\" \"2001:db8::1/64\""));
        EXPECT_TRUE(commandWasIssued("address \"del\" \"198.51.100.1/24\""));
        EXPECT_TRUE(Ethernet0IPv6Set);
        std::vector<swss::FieldValueTuple> values;
        EXPECT_FALSE(appIntfs.get("Ethernet0:198.51.100.1/24", values));
    }


    TEST_F(IntfMgrTest, SagPublicationCannotOverrideOrRaiseAHeldTransition)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Vlan100", {{"state", "ok"}});
        mgr.m_stateVlanTable.set("Vlan100", {{"state", "ok"}});
        mgr.m_stateVrfTable.set("VrfRed", {{"state", "ok"}});
        mgr.m_stateIntfTable.set("Vlan100", {{"vrf", "VrfBlue"}});
        mgr.m_cfgVlanIntfTable.set("Vlan100", {{"static_anycast_gateway", "true"},
            {"vrf_name", "VrfRed"}});
        mgr.m_sagIntfList["Vlan100"] = true;
        mgr.m_rehomeStateTable.set("__owner__", {{"epoch", "owner"}});
        std::string target = "VrfRed";
        ASSERT_FALSE(mgr.processRehome("Vlan100", target));
        auto &request = mgr.m_rehomeRequests.at("Vlan100");
        mgr.m_rehomeStateTable.set("Vlan100", {{"owner_id", request.id},
            {"owner_epoch", "owner"}, {"owner_phase", "waiting"}});
        ASSERT_FALSE(mgr.processRehome("Vlan100", target));
        ASSERT_EQ(request.phase, "drain");
        ASSERT_TRUE(request.quiesced);

        swss::Table appIntfs(m_app_db.get(), APP_INTF_TABLE_NAME);
        std::string published, phase;
        ASSERT_TRUE(appIntfs.hget("Vlan100", "vrf_name", published));
        ASSERT_TRUE(appIntfs.hget("Vlan100", "rehome_phase", phase));
        ASSERT_EQ(published, "VrfBlue");
        ASSERT_EQ(phase, "prepare");
        mockCallArgs.clear();

        mgr.updateSagMac("02:03:04:05:06:07");

        std::string value;
        ASSERT_TRUE(appIntfs.hget("Vlan100", "mac_addr", value));
        EXPECT_EQ(value, "02:03:04:05:06:07");
        ASSERT_TRUE(appIntfs.hget("Vlan100", "vrf_name", value));
        EXPECT_EQ(value, published);
        ASSERT_TRUE(appIntfs.hget("Vlan100", "rehome_phase", value));
        EXPECT_EQ(value, phase);
        EXPECT_FALSE(commandWasIssued("\"Vlan100\" up"));
        EXPECT_TRUE(commandWasIssued("\"Vlan100\" down"));

        mgr.m_cfgVlanIntfTable.set("Vlan200", {{"static_anycast_gateway", "true"},
            {"vrf_name", "VrfRed"}, {"mtu", "9100"}});
        mgr.m_sagIntfList["Vlan200"] = true;
        mockCallArgs.clear();
        mgr.updateSagMac("02:03:04:05:06:08");
        ASSERT_TRUE(appIntfs.hget("Vlan200", "vrf_name", value));
        EXPECT_EQ(value, "VrfRed");
        ASSERT_TRUE(appIntfs.hget("Vlan200", "mtu", value));
        EXPECT_EQ(value, "9100");
        EXPECT_TRUE(commandWasIssued("\"Vlan200\" down"));
        EXPECT_TRUE(commandWasIssued("\"Vlan200\" up"));
    }


    struct ProducerStagingGuard
    {
        ProducerStagingGuard() { testing_db::enableProducerStaging(true); }
        ~ProducerStagingGuard()
        {
            testing_db::discardProducerStaging();
            testing_db::enableProducerStaging(false);
        }
    };

    struct RehomeClearTest : IntfMgrTest
    {
        const std::vector<swss::FieldValueTuple> request = {{"vrf_name", "VrfRed"}};

        std::string materialized(const std::string &field)
        {
            swss::Table appIntfs(m_app_db.get(), APP_INTF_TABLE_NAME);
            std::string value;
            appIntfs.hget("Ethernet0", field, value);
            return value;
        }

        std::string rehomeState(const std::string &field)
        {
            swss::Table state(m_state_db.get(), "INTERFACE_REHOME_TABLE");
            std::string value;
            state.hget("Ethernet0", field, value);
            return value;
        }

        size_t ownerConsumes()
        {
            return testing_db::popProducerStaging(m_app_db->getDbId(), APP_INTF_TABLE_NAME).size();
        }

        size_t unconsumedUpdates()
        {
            return testing_db::stagedProducerUpdates(m_app_db->getDbId(), APP_INTF_TABLE_NAME);
        }

        void driveToRelease(swss::IntfMgr &mgr)
        {
            mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
            mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", "VrfBlue"}});
            mgr.m_stateVrfTable.set("VrfRed", {{"state", "ok"}});
            mgr.m_rehomeStateTable.set("__owner__", {{"epoch", "owner"}});
            mgr.m_cfgIntfTable.set("Ethernet0", {{"vrf_name", "VrfRed"}});
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, request, SET_COMMAND));
            const auto id = mgr.m_rehomeRequests.at("Ethernet0").id;
            ownerConsumes();
            mgr.m_rehomeStateTable.set("Ethernet0", {{"owner_id", id},
                {"owner_epoch", "owner"}, {"owner_phase", "waiting"}});
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, request, SET_COMMAND));
            ownerConsumes();
            mgr.m_rehomeStateTable.hset("Ethernet0", "owner_phase", "ready");
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, request, SET_COMMAND));
            ownerConsumes();
            mgr.m_rehomeStateTable.hset("Ethernet0", "owner_phase", "applied");
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, request, SET_COMMAND));
            ASSERT_EQ(mgr.m_rehomeRequests.at("Ethernet0").phase, "release");
            ASSERT_EQ(ownerConsumes(), 1u);
            ASSERT_EQ(materialized("rehome_phase"), "release");
            ASSERT_EQ(materialized("rehome_epoch"), "owner");
            mgr.m_rehomeStateTable.hset("Ethernet0", "owner_phase", "released");
        }
    };

    TEST_F(RehomeClearTest, TerminalOutcomeAwaitsConsumedControlClear)
    {
        ProducerStagingGuard staging;
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        driveToRelease(mgr);
        ASSERT_FALSE(HasFatalFailure());

        const bool finished = mgr.doIntfGeneralTask({"Ethernet0"}, request, SET_COMMAND);

        ASSERT_EQ(materialized("rehome_phase"), "release");
        ASSERT_EQ(materialized("rehome_epoch"), "owner");
        EXPECT_GE(unconsumedUpdates(), 1u);
        EXPECT_FALSE(finished);
        EXPECT_EQ(rehomeState("outcome"), "pending");
        EXPECT_NE(rehomeState("manager_phase"), "done");
        EXPECT_EQ(mgr.m_rehomeRequests.count("Ethernet0"), 1u);
        std::string applied;
        EXPECT_TRUE(mgr.m_stateIntfTable.hget("Ethernet0", "vrf", applied));
        EXPECT_EQ(applied, "VrfRed");

        mockCallArgs.clear();
        mgr.m_rehomeRequests.at("Ethernet0").deadline = std::chrono::steady_clock::now();
        mgr.m_rehomeStateTable.set("__owner__", {{"epoch", "restarted-owner"}});
        for (unsigned int pass = 0; pass < 3; ++pass)
        {
            EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, request, SET_COMMAND));
        }
        EXPECT_FALSE(commandWasIssued("keep_addr_on_down=1"));
        EXPECT_FALSE(commandWasIssued("master \"VrfBlue\""));
        EXPECT_NE(rehomeState("manager_phase"), "recover");

        ASSERT_GE(ownerConsumes(), 1u);
        EXPECT_EQ(materialized("rehome_phase"), "");
        EXPECT_EQ(materialized("rehome_epoch"), "");
        EXPECT_EQ(materialized("vrf_name"), "VrfRed");
        EXPECT_TRUE(mgr.doIntfGeneralTask({"Ethernet0"}, request, SET_COMMAND));
        EXPECT_EQ(rehomeState("outcome"), "succeeded");
        EXPECT_EQ(rehomeState("manager_applied_vrf"), "VrfRed");
        EXPECT_TRUE(mgr.m_rehomeRequests.empty());
    }

    TEST_F(RehomeClearTest, InterruptedClearIsResumedAfterManagerRestart)
    {
        ProducerStagingGuard staging;
        {
            swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
            driveToRelease(mgr);
            ASSERT_FALSE(HasFatalFailure());
            mgr.doIntfGeneralTask({"Ethernet0"}, request, SET_COMMAND);
        }
        testing_db::discardProducerStaging();
        ASSERT_EQ(materialized("rehome_phase"), "release");
        ASSERT_EQ(materialized("rehome_epoch"), "owner");

        swss::IntfMgr restarted(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mockCallArgs.clear();
        EXPECT_FALSE(restarted.doIntfGeneralTask({"Ethernet0"}, request, SET_COMMAND));
        EXPECT_FALSE(commandWasIssued("keep_addr_on_down=1"));
        ASSERT_GE(unconsumedUpdates(), 1u) << "restarted manager republished nothing";
        ASSERT_GE(ownerConsumes(), 1u);
        EXPECT_EQ(materialized("rehome_phase"), "");
        EXPECT_EQ(materialized("vrf_name"), "VrfRed");
        EXPECT_TRUE(restarted.doIntfGeneralTask({"Ethernet0"}, request, SET_COMMAND));
        EXPECT_EQ(rehomeState("outcome"), "succeeded");
        EXPECT_TRUE(restarted.m_rehomeRequests.empty());
    }

    TEST_F(RehomeClearTest, DeletionDischargesAnOutstandingClear)
    {
        ProducerStagingGuard staging;
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        driveToRelease(mgr);
        ASSERT_FALSE(HasFatalFailure());
        ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, request, SET_COMMAND));
        ASSERT_EQ(mgr.m_rehomeRequests.count("Ethernet0"), 1u);

        EXPECT_TRUE(mgr.doIntfGeneralTask({"Ethernet0"}, {}, DEL_COMMAND));
        EXPECT_TRUE(mgr.m_rehomeRequests.empty());
        EXPECT_EQ(rehomeState("outcome"), "");
        ASSERT_GE(ownerConsumes(), 1u);
        std::vector<swss::FieldValueTuple> values;
        swss::Table appIntfs(m_app_db.get(), APP_INTF_TABLE_NAME);
        EXPECT_FALSE(appIntfs.get("Ethernet0", values));
    }

    TEST_F(IntfMgrTest, testSettingIpv6Flag){
        Ethernet0IPv6Set = false;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        /* Set portStateTable */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        /* Set m_stateIntfTable */
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");
        /* Set Ipv6 prefix */
        const std::vector<std::string>& keys = {"Ethernet0", "2001::8/64"};
        const std::vector<swss::FieldValueTuple> data;
        intfmgr.doIntfAddrTask(keys, data, "SET");
        int ip_cmd_called = 0;
        for (auto cmd : mockCallArgs){
            if (cmd.find("/sbin/ip -6 address \"add\"") == 0){
                ip_cmd_called++;
            }
        }
        ASSERT_EQ(ip_cmd_called, 2);
    }

    TEST_F(IntfMgrTest, testNoSettingIpv6Flag){
        Ethernet0IPv6Set = true; // Assuming it is already set by SDK
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        /* Set portStateTable */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        /* Set m_stateIntfTable */
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");
        /* Set Ipv6 prefix */
        const std::vector<std::string>& keys = {"Ethernet0", "2001::8/64"};
        const std::vector<swss::FieldValueTuple> data;
        intfmgr.doIntfAddrTask(keys, data, "SET");
        int ip_cmd_called = 0;
        for (auto cmd : mockCallArgs){
            if (cmd.find("/sbin/ip -6 address \"add\"") == 0){
                ip_cmd_called++;
            }
        }
        ASSERT_EQ(ip_cmd_called, 1);
    }

    //This test except no runtime error when the set admin status command failed
    //and the subinterface has not ok status (for example not existing subinterface)
    TEST_F(IntfMgrTest, testSetAdminStatusFailToNotOkSubInt){
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        intfmgr.setHostSubIntfAdminStatus("Ethernet64.10", "up", "up");
    }

    //This test except runtime error when the set admin status command failed
    //and the subinterface has ok status
    TEST_F(IntfMgrTest, testSetAdminStatusFailToOkSubInt){
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        /* Set portStateTable */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet64.10", values, "SET", "");
        EXPECT_THROW(intfmgr.setHostSubIntfAdminStatus("Ethernet64.10", "up", "up"), std::runtime_error);
    }

    TEST_F(IntfMgrTest, testReplayLLIpv6AddressOnAdminUp){
        Ethernet0IPv6Set = true;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        /* Set portStateTable and stateIntfTable so doIntfAddrTask proceeds */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");

        /* Add an IPv6 link-local address via doIntfAddrTask to populate the cache */
        const std::vector<std::string> llKeys = {"Ethernet0", "fe80::1/64"};
        const std::vector<swss::FieldValueTuple> emptyData;
        intfmgr.doIntfAddrTask(llKeys, emptyData, "SET");

        /* Also add a global IPv6 address — this should NOT be replayed */
        const std::vector<std::string> globalKeys = {"Ethernet0", "2001::8/64"};
        intfmgr.doIntfAddrTask(globalKeys, emptyData, "SET");

        /* Also add an IPv4 address — this should NOT be replayed */
        const std::vector<std::string> ipv4Keys = {"Ethernet0", "10.0.0.1/31"};
        intfmgr.doIntfAddrTask(ipv4Keys, emptyData, "SET");

        mockCallArgs.clear();

        /* Simulate admin up by calling doPortTableTask */
        std::vector<swss::FieldValueTuple> portData;
        portData.emplace_back("admin_status", "up");
        intfmgr.doPortTableTask("Ethernet0", portData, "SET");

        /* Verify that only IPv6 link-local address add was called */
        int ipv6_ll_add_called = 0;
        int ipv6_global_add_called = 0;
        int ipv4_add_called = 0;
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos &&
                cmd.find("fe80::1/64") != std::string::npos)
            {
                ipv6_ll_add_called++;
            }
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos &&
                cmd.find("2001::8/64") != std::string::npos)
            {
                ipv6_global_add_called++;
            }
            if (cmd.find("/sbin/ip address \"add\"") != std::string::npos &&
                cmd.find("10.0.0.1/31") != std::string::npos)
            {
                ipv4_add_called++;
            }
        }
        ASSERT_EQ(ipv6_ll_add_called, 1);
        ASSERT_EQ(ipv6_global_add_called, 0);
        ASSERT_EQ(ipv4_add_called, 0);

        /* Now delete the link-local address and verify it is no longer replayed */
        intfmgr.doIntfAddrTask(llKeys, emptyData, "DEL");
        ASSERT_EQ(intfmgr.m_intfLLAddresses.count("Ethernet0"), 0u);

        mockCallArgs.clear();
        intfmgr.doPortTableTask("Ethernet0", portData, "SET");

        ipv6_ll_add_called = 0;
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos &&
                cmd.find("fe80::1/64") != std::string::npos)
            {
                ipv6_ll_add_called++;
            }
        }
        ASSERT_EQ(ipv6_ll_add_called, 0);
    }

    TEST_F(IntfMgrTest, testNoReplayLLOnAdminDown){
        Ethernet0IPv6Set = true;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        /* Set portStateTable and stateIntfTable so doIntfAddrTask proceeds */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");

        /* Add an IPv6 link-local address via doIntfAddrTask to populate the cache */
        const std::vector<std::string> llKeys = {"Ethernet0", "fe80::1/64"};
        const std::vector<swss::FieldValueTuple> emptyData;
        intfmgr.doIntfAddrTask(llKeys, emptyData, "SET");

        mockCallArgs.clear();

        /* Simulate admin down — should NOT trigger replay */
        std::vector<swss::FieldValueTuple> portData;
        portData.emplace_back("admin_status", "down");
        intfmgr.doPortTableTask("Ethernet0", portData, "SET");

        int ipv6_add_called = 0;
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos)
            {
                ipv6_add_called++;
            }
        }
        ASSERT_EQ(ipv6_add_called, 0);
    }

    TEST_F(IntfMgrTest, testSetSagFdbEntryValidationAndBridgeCommand){
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        mockCallArgs.clear();
        intfmgr.setSagFdbEntry("update", "Vlan100", "02:03:04:05:06:07");
        intfmgr.setSagFdbEntry("replace", "Ethernet0", "02:03:04:05:06:07");
        intfmgr.setSagFdbEntry("replace", "VlanABC", "02:03:04:05:06:07");
        intfmgr.setSagFdbEntry("replace", "Vlan100", gMacAddress.to_string());
        EXPECT_TRUE(mockCallArgs.empty());

        FailBridgeFdbCommand = true;
        intfmgr.setSagFdbEntry("replace", "Vlan100", "02:03:04:05:06:07");
        ASSERT_EQ(mockCallArgs.size(), 1u);
        EXPECT_EQ(mockCallArgs[0], "bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent");
    }

    TEST_F(IntfMgrTest, testUpdateSagMacProgramsSagVlans){
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        gSagMacAddress = swss::MacAddress("00:aa:bb:cc:dd:ee");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        intfmgr.m_cfgVlanIntfTable.set("Vlan100", {
            {"static_anycast_gateway", "true"},
            {"proxy_arp", "enabled"}
        });
        intfmgr.m_cfgVlanIntfTable.set("Vlan200", {
            {"static_anycast_gateway", "false"}
        });
        intfmgr.m_cfgVlanIntfTable.set("Vlan300|10.0.0.1/24", {
            {"static_anycast_gateway", "true"}
        });

        mockCallArgs.clear();
        intfmgr.updateSagMac("02:03:04:05:06:07");

        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" down"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 02:03:04:05:06:07"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" up"));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 00:aa:bb:cc:dd:ee dev Bridge vlan 100 permanent"));
        EXPECT_TRUE(commandWasIssued("bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));
        EXPECT_FALSE(commandWasIssued("Vlan200 address"));
        EXPECT_FALSE(commandWasIssued("Vlan300"));

        swss::Table appIntfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(appIntfTable.get("Vlan100", values));
        std::string mac;
        ASSERT_TRUE(getFieldValue(values, "mac_addr", mac));
        EXPECT_EQ(mac, "02:03:04:05:06:07");
    }

    TEST_F(IntfMgrTest, testDoSagTaskSetAndDelete){
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        gSagMacAddress = swss::MacAddress("00:00:00:00:00:00");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        intfmgr.m_cfgVlanIntfTable.set("Vlan100", {
            {"static_anycast_gateway", "true"}
        });

        const std::vector<std::string> keys = {"GLOBAL"};
        mockCallArgs.clear();
        intfmgr.doSagTask(keys, {}, SET_COMMAND);
        EXPECT_TRUE(mockCallArgs.empty());

        intfmgr.doSagTask(keys, {{"gateway_mac", "02:03:04:05:06:07"}}, SET_COMMAND);
        EXPECT_TRUE(commandWasIssued("bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));

        swss::Table appSagTable(m_app_db.get(), APP_SAG_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(appSagTable.get("GLOBAL", values));
        std::string mac;
        ASSERT_TRUE(getFieldValue(values, "gateway_mac", mac));
        EXPECT_EQ(mac, "02:03:04:05:06:07");

        mockCallArgs.clear();
        intfmgr.doSagTask(keys, {}, DEL_COMMAND);
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 00:11:22:33:44:55"));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));
        EXPECT_FALSE(commandWasIssued("bridge fdb replace 00:11:22:33:44:55"));
        EXPECT_FALSE(appSagTable.get("GLOBAL", values));

        intfmgr.doSagTask(keys, {}, "UNKNOWN");
    }

    TEST_F(IntfMgrTest, testDoIntfGeneralTaskStaticAnycastGateway)
    {
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        gSagMacAddress = swss::MacAddress("00:aa:bb:cc:dd:ee");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        intfmgr.m_stateVlanTable.set("Vlan100", {{"state", "ok"}}, "SET", "");
        intfmgr.m_stateVlanTable.set("Vlan200", {{"state", "ok"}}, "SET", "");
        intfmgr.m_cfgSagTable.set("GLOBAL", {{"gateway_mac", "02:03:04:05:06:07"}});

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan100"}, {{"static_anycast_gateway", "true"}}, SET_COMMAND));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" down"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 02:03:04:05:06:07"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" up"));
        EXPECT_TRUE(commandWasIssued("bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));
        EXPECT_TRUE(intfmgr.m_sagIntfList.at("Vlan100"));

        swss::Table appIntfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(appIntfTable.get("Vlan100", values));
        std::string mac;
        ASSERT_TRUE(getFieldValue(values, "mac_addr", mac));
        EXPECT_EQ(mac, "02:03:04:05:06:07");

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan100"}, {}, DEL_COMMAND));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 00:aa:bb:cc:dd:ee dev Bridge vlan 100 permanent"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 00:11:22:33:44:55"));
        EXPECT_EQ(intfmgr.m_sagIntfList.count("Vlan100"), 0u);

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan200"}, {{"static_anycast_gateway", "false"}}, SET_COMMAND));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 00:aa:bb:cc:dd:ee dev Bridge vlan 200 permanent"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan200 address 00:11:22:33:44:55"));
        ASSERT_TRUE(appIntfTable.get("Vlan200", values));
        ASSERT_TRUE(getFieldValue(values, "mac_addr", mac));
        EXPECT_EQ(mac, swss::MacAddress().to_string());

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan200"}, {{"static_anycast_gateway", "invalid"}}, SET_COMMAND));
        EXPECT_FALSE(commandWasIssued("bridge fdb"));
    }

}
