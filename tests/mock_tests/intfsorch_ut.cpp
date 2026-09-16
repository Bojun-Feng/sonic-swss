#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#include "gtest/gtest.h"
#include "ut_helper.h"
#include "swssnet.h"
#include "sai_serialize.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include <memory>
#include <vector>

extern sai_mpls_api_t *sai_mpls_api;


namespace intfsorch_test
{
    using namespace std;

    int create_rif_count = 0;
    int remove_rif_count = 0;
    bool saw_loopback_action = false;
    bool fail_next_rif_set = false;
    bool fail_next_rif_create = false;
    bool fail_next_rif_remove = false;
    int loopback_action_set_count = 0;
    sai_packet_action_t last_loopback_action = SAI_PACKET_ACTION_FORWARD;
    sai_router_interface_api_t *pold_sai_rif_api;
    sai_router_interface_api_t ut_sai_rif_api;

    sai_status_t _ut_create_router_interface(
            _Out_ sai_object_id_t *router_interface_id,
            _In_ sai_object_id_t switch_id,
            _In_ uint32_t attr_count,
            _In_ const sai_attribute_t *attr_list)
    {
        ++create_rif_count;
        if (fail_next_rif_create)
        {
            fail_next_rif_create = false;
            return SAI_STATUS_INSUFFICIENT_RESOURCES;
        }
        return pold_sai_rif_api->create_router_interface(
            router_interface_id, switch_id, attr_count, attr_list);
    }

    sai_status_t _ut_remove_router_interface(
            _In_ sai_object_id_t router_interface_id)
    {
        ++remove_rif_count;
        if (fail_next_rif_remove)
        {
            fail_next_rif_remove = false;
            return SAI_STATUS_OBJECT_IN_USE;
        }
        return pold_sai_rif_api->remove_router_interface(router_interface_id);
    }

    int hostif_keep_tag_count = 0;
    int hostif_strip_tag_count = 0;
    bool fail_next_hostif_vlan_tag_set = false;
    sai_hostif_api_t *pold_sai_hostif_api;
    sai_hostif_api_t ut_sai_hostif_api;

    sai_status_t _ut_set_hostif_attribute(
            _In_ sai_object_id_t hif_id,
            _In_ const sai_attribute_t *attr)
    {
        if (attr->id == SAI_HOSTIF_ATTR_VLAN_TAG && fail_next_hostif_vlan_tag_set)
        {
            fail_next_hostif_vlan_tag_set = false;
            return SAI_STATUS_INSUFFICIENT_RESOURCES;
        }
        const auto status = pold_sai_hostif_api->set_hostif_attribute(hif_id, attr);
        if (attr->id == SAI_HOSTIF_ATTR_VLAN_TAG && status == SAI_STATUS_SUCCESS)
        {
            if (attr->value.s32 == SAI_HOSTIF_VLAN_TAG_KEEP)
            {
                ++hostif_keep_tag_count;
            }
            else if (attr->value.s32 == SAI_HOSTIF_VLAN_TAG_STRIP)
            {
                ++hostif_strip_tag_count;
            }
        }
        return status;
    }

    sai_status_t _ut_set_router_interface_attribute(
            _In_ sai_object_id_t router_interface_id,
            _In_ const sai_attribute_t *attr)
    {
        if (attr->id == SAI_ROUTER_INTERFACE_ATTR_LOOPBACK_PACKET_ACTION)
        {
            ++loopback_action_set_count;
            last_loopback_action = static_cast<sai_packet_action_t>(attr->value.s32);
            if (fail_next_rif_set)
            {
                fail_next_rif_set = false;
                return SAI_STATUS_INSUFFICIENT_RESOURCES;
            }
            saw_loopback_action = true;
            return SAI_STATUS_SUCCESS;
        }
        return pold_sai_rif_api->set_router_interface_attribute(
            router_interface_id, attr);
    }

    struct IntfsOrchTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;

        //sai_router_interface_api_t *old_sai_rif_api_ptr;

        //sai_create_router_interface_fn old_create_rif;
        //sai_remove_router_interface_fn old_remove_rif;
        void SetUp() override
        {
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);
            pold_sai_rif_api = sai_router_intfs_api;
            ut_sai_rif_api = *sai_router_intfs_api;
            sai_router_intfs_api = &ut_sai_rif_api;

            sai_router_intfs_api->create_router_interface = _ut_create_router_interface;
            sai_router_intfs_api->remove_router_interface = _ut_remove_router_interface;
            sai_router_intfs_api->set_router_interface_attribute = _ut_set_router_interface_attribute;
            pold_sai_hostif_api = sai_hostif_api;
            ut_sai_hostif_api = *sai_hostif_api;
            sai_hostif_api = &ut_sai_hostif_api;
            sai_hostif_api->set_hostif_attribute = _ut_set_hostif_attribute;
            hostif_keep_tag_count = 0;
            hostif_strip_tag_count = 0;
            fail_next_hostif_vlan_tag_set = false;
            saw_loopback_action = false;
            fail_next_rif_set = false;
            fail_next_rif_create = false;
            fail_next_rif_remove = false;
            loopback_action_set_count = 0;
            last_loopback_action = SAI_PACKET_ACTION_FORWARD;

            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);

            sai_attribute_t attr;

            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            auto status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            // Get switch source MAC address
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);

            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gMacAddress = attr.value.mac;

            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);

            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gVirtualRouterId = attr.value.oid;


            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector app_switch_table(m_app_db.get(),  APP_SWITCH_TABLE_NAME);

            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            // Create dependencies ...
            TableConnector stateDbBfdSessionTable(m_state_db.get(), STATE_BFD_SESSION_TABLE_NAME);
            gBfdOrch = new BfdOrch(m_app_db.get(), APP_BFD_SESSION_TABLE_NAME, stateDbBfdSessionTable);

            const int portsorch_base_pri = 40;
            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            vector<string> flex_counter_tables = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());

            vector<string> vnet_tables = {
                APP_VNET_RT_TABLE_NAME,
                APP_VNET_RT_TUNNEL_TABLE_NAME
            };

            vector<string> cfg_vnet_tables = {
                CFG_VNET_RT_TABLE_NAME,
                CFG_VNET_RT_TUNNEL_TABLE_NAME
            };

            auto* vnet_orch = new VNetOrch(m_app_db.get(), APP_VNET_TABLE_NAME);
            gDirectory.set(vnet_orch);
            auto* cfg_vnet_rt_orch = new VNetCfgRouteOrch(m_config_db.get(), m_app_db.get(), cfg_vnet_tables);
            gDirectory.set(cfg_vnet_rt_orch);
            auto* vnet_rt_orch = new VNetRouteOrch(m_app_db.get(), vnet_tables, vnet_orch);
            gDirectory.set(vnet_rt_orch);
            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);
            gDirectory.set(gVrfOrch);

            vector<table_name_with_pri_t> intf_tables = {
                { APP_INTF_TABLE_NAME,  IntfsOrch::intfsorch_pri},
                { APP_SAG_TABLE_NAME,   IntfsOrch::intfsorch_pri}
            };

            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_app_db.get(), intf_tables, gVrfOrch, m_chassis_app_db.get());

            const int fdborch_pri = 20;

            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri},
                { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri},
                { APP_MCLAG_FDB_TABLE_NAME,  fdborch_pri}
            };

            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            ASSERT_EQ(gFdbOrch, nullptr);
            gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch,
                                   m_config_db.get());

            ASSERT_EQ(gNeighOrch, nullptr);
            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, gPortsOrch, m_chassis_app_db.get());

            vector<string> tunnel_tables = {
                APP_TUNNEL_DECAP_TABLE_NAME,
                APP_TUNNEL_DECAP_TERM_TABLE_NAME
            };
            auto* tunnel_decap_orch = new TunnelDecapOrch(m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
            gTunneldecapOrch = tunnel_decap_orch;
            vector<string> mux_tables = {
                CFG_MUX_CABLE_TABLE_NAME,
                CFG_PEER_SWITCH_TABLE_NAME
            };
            auto* mux_orch = new MuxOrch(m_config_db.get(), mux_tables, tunnel_decap_orch, gNeighOrch, gFdbOrch);
            gDirectory.set(mux_orch);

            ASSERT_EQ(gFgNhgOrch, nullptr);
            const int fgnhgorch_pri = 15;

            vector<table_name_with_pri_t> fgnhg_tables = {
                { CFG_FG_NHG,                 fgnhgorch_pri },
                { CFG_FG_NHG_PREFIX,          fgnhgorch_pri },
                { CFG_FG_NHG_MEMBER,          fgnhgorch_pri }
            };
            gFgNhgOrch = new FgNhgOrch(m_config_db.get(), m_app_db.get(), m_state_db.get(), fgnhg_tables, gNeighOrch, gIntfsOrch, gVrfOrch);

            ASSERT_EQ(gSrv6Orch, nullptr);
            TableConnector srv6_sid_list_table(m_app_db.get(), APP_SRV6_SID_LIST_TABLE_NAME);
            TableConnector srv6_my_sid_table(m_app_db.get(), APP_SRV6_MY_SID_TABLE_NAME);
            TableConnector srv6_my_sid_cfg_table(m_config_db.get(), CFG_SRV6_MY_SID_TABLE_NAME);

            vector<TableConnector> srv6_tables = {
                srv6_sid_list_table,
                srv6_my_sid_table,
                srv6_my_sid_cfg_table
            };
            gSrv6Orch = new Srv6Orch(m_config_db.get(), m_app_db.get(), srv6_tables, gSwitchOrch, gVrfOrch, gNeighOrch);

            // Start FlowCounterRouteOrch
            static const  vector<string> route_pattern_tables = {
                CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
            };
            gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);

            ASSERT_EQ(gRouteOrch, nullptr);
            const int routeorch_pri = 5;
            vector<table_name_with_pri_t> route_tables = {
                { APP_ROUTE_TABLE_NAME,        routeorch_pri },
                { APP_LABEL_ROUTE_TABLE_NAME,  routeorch_pri }
            };
            gRouteOrch = new RouteOrch(m_app_db.get(), route_tables, gSwitchOrch, gNeighOrch, gIntfsOrch, gVrfOrch, gFgNhgOrch, gSrv6Orch);
            gNhgOrch = new NhgOrch(m_app_db.get(), APP_NEXTHOP_GROUP_TABLE_NAME);

            // Recreate buffer orch to read populated data
            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };

            gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

            Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

            // Get SAI default ports to populate DB
            auto ports = ut_helper::getInitialSaiPorts();

            // Populate pot table with SAI ports
            for (const auto &it : ports)
            {
                portTable.set(it.first, it.second);
            }
            // Set PortConfigDone
            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();
            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

        void TearDown() override
        {
            gDirectory.m_values.clear();

            delete gCrmOrch;
            gCrmOrch = nullptr;

            delete gSwitchOrch;
            gSwitchOrch = nullptr;

            delete gBfdOrch;
            gBfdOrch = nullptr;

            delete gSrv6Orch;
            gSrv6Orch = nullptr;

            delete gNeighOrch;
            gNeighOrch = nullptr;

            delete gFdbOrch;
            gFdbOrch = nullptr;

            delete gPortsOrch;
            gPortsOrch = nullptr;

            delete gIntfsOrch;
            gIntfsOrch = nullptr;

            delete gFgNhgOrch;
            gFgNhgOrch = nullptr;

            delete gRouteOrch;
            gRouteOrch = nullptr;

            delete gTunneldecapOrch;
            gTunneldecapOrch = nullptr;

            delete gNhgOrch;
            gNhgOrch = nullptr;

            delete gBufferOrch;
            gBufferOrch = nullptr;

            delete gVrfOrch;
            gVrfOrch = nullptr;

            delete gFlowCounterRouteOrch;
            gFlowCounterRouteOrch = nullptr;

            sai_router_intfs_api = pold_sai_rif_api;
            sai_hostif_api = pold_sai_hostif_api;
            ut_helper::uninitSaiApi();
        }
    };



    static std::string ownerEpoch(swss::Table &status)
    {
        std::string epoch;
        EXPECT_TRUE(status.hget("__owner__", "epoch", epoch));
        return epoch;
    }

    static void openRehomeRequest(swss::Table &status, const std::string &alias,
                                  const std::string &id, const std::string &target)
    {
        status.set(alias, {{"manager_id", id}, {"manager_target", target}, {"outcome", "pending"}});
    }

    static KeyOpFieldsValuesTuple rehomeRow(const std::string &alias, const std::string &id,
                                            const std::string &phase, const std::string &vrf,
                                            const std::string &target, const std::string &epoch)
    {
        return KeyOpFieldsValuesTuple(alias, SET_COMMAND,
            {{"vrf_name", vrf}, {"rehome_id", id}, {"rehome_phase", phase},
             {"rehome_target", target}, {"rehome_epoch", epoch}});
    }

    struct RehomeOwnerTest : IntfsOrchTest
    {
        Consumer *interfaces = nullptr;
        std::unique_ptr<swss::Table> status;
        std::string epoch;

        void SetUp() override
        {
            IntfsOrchTest::SetUp();
            auto vrfs = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
            ASSERT_NE(vrfs, nullptr);
            vrfs->addToSync(KeyOpFieldsValuesTuple("VrfBlue", SET_COMMAND, {{"empty", "empty"}}));
            vrfs->addToSync(KeyOpFieldsValuesTuple("VrfRed", SET_COMMAND, {{"empty", "empty"}}));
            static_cast<Orch *>(gVrfOrch)->doTask();
            ASSERT_TRUE(gVrfOrch->isVRFexists("VrfBlue"));
            ASSERT_TRUE(gVrfOrch->isVRFexists("VrfRed"));
            interfaces = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
            ASSERT_NE(interfaces, nullptr);
            status = std::make_unique<swss::Table>(m_state_db.get(), "INTERFACE_REHOME_TABLE");
            epoch = ownerEpoch(*status);
            for (const auto &alias : {"Ethernet0", "Ethernet0.10"})
            {
                status->del(alias);
            }
        }

        sai_object_id_t admitMoveToVrfRed(const std::string &alias)
        {
            interfaces->addToSync(KeyOpFieldsValuesTuple(alias, SET_COMMAND, {{"vrf_name", ""}}));
            static_cast<Orch *>(gIntfsOrch)->doTask();
            const auto oldRif = gIntfsOrch->getRouterIntfsId(alias);
            EXPECT_NE(oldRif, SAI_NULL_OBJECT_ID);
            openRehomeRequest(*status, alias, "req-1", "VrfRed");
            interfaces->addToSync(rehomeRow(alias, "req-1", "prepare", "", "VrfRed", epoch));
            static_cast<Orch *>(gIntfsOrch)->doTask();
            EXPECT_TRUE(gIntfsOrch->isIntfChangeInProgress(alias));
            EXPECT_EQ(gIntfsOrch->getRouterIntfsId(alias), oldRif);
            return oldRif;
        }

        void applyAndRelease(const std::string &alias)
        {
            interfaces->addToSync(rehomeRow(alias, "req-1", "apply", "VrfRed", "VrfRed", epoch));
            static_cast<Orch *>(gIntfsOrch)->doTask();
            interfaces->addToSync(rehomeRow(alias, "req-1", "release", "VrfRed", "VrfRed", epoch));
            static_cast<Orch *>(gIntfsOrch)->doTask();
        }
    };

    TEST_F(RehomeOwnerTest, AdmissionReportsReadinessAndAppliesTheTargetRif)
    {
        const auto oldRif = admitMoveToVrfRed("Ethernet0");
        ASSERT_FALSE(HasFatalFailure());
        std::string ownerPhase, refCount;
        ASSERT_TRUE(status->hget("Ethernet0", "owner_phase", ownerPhase));
        ASSERT_TRUE(status->hget("Ethernet0", "ref_count", refCount));
        EXPECT_EQ(ownerPhase, "ready");
        EXPECT_EQ(refCount, "0");
        EXPECT_FALSE(interfaces->m_toSync.empty());

        applyAndRelease("Ethernet0");
        EXPECT_TRUE(interfaces->m_toSync.empty());
        EXPECT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        const auto newRif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(newRif, SAI_NULL_OBJECT_ID);
        EXPECT_NE(newRif, oldRif);
        sai_attribute_t attr{};
        attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
        ASSERT_EQ(sai_router_intfs_api->get_router_interface_attribute(newRif, 1, &attr), SAI_STATUS_SUCCESS);
        EXPECT_EQ(attr.value.oid, gVrfOrch->getVRFid("VrfRed"));
        EXPECT_NE(sai_router_intfs_api->get_router_interface_attribute(oldRif, 1, &attr), SAI_STATUS_SUCCESS);
        ASSERT_TRUE(status->hget("Ethernet0", "owner_phase", ownerPhase));
        EXPECT_EQ(ownerPhase, "released");
    }

    TEST_F(RehomeOwnerTest, UnarbitratedVrfFieldNeverReplacesTheAppliedRif)
    {
        interfaces->addToSync(KeyOpFieldsValuesTuple("Ethernet0", SET_COMMAND, {{"vrf_name", "VrfBlue"}}));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        const auto blueRif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(blueRif, SAI_NULL_OBJECT_ID);

        interfaces->addToSync(KeyOpFieldsValuesTuple("Ethernet0", SET_COMMAND,
            {{"vrf_name", "VrfRed"}, {"mac_addr", "02:03:04:05:06:07"},
             {"rehome_id", ""}, {"rehome_phase", ""}, {"rehome_target", ""}, {"rehome_epoch", ""}}));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        EXPECT_TRUE(interfaces->m_toSync.empty());
        EXPECT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        EXPECT_EQ(gIntfsOrch->getRouterIntfsId("Ethernet0"), blueRif);
        EXPECT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").vrf_id, gVrfOrch->getVRFid("VrfBlue"));
    }

    TEST_F(RehomeOwnerTest, CommandFromAPreviousOwnerEpochIsNotActedOn)
    {
        interfaces->addToSync(KeyOpFieldsValuesTuple("Ethernet0", SET_COMMAND, {{"vrf_name", ""}}));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        const auto rif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(rif, SAI_NULL_OBJECT_ID);

        openRehomeRequest(*status, "Ethernet0", "req-1", "VrfRed");
        interfaces->addToSync(rehomeRow("Ethernet0", "req-1", "prepare", "", "VrfRed",
                                        "a-previous-owner"));
        for (unsigned int pass = 0; pass < 3; ++pass)
        {
            static_cast<Orch *>(gIntfsOrch)->doTask();
        }
        EXPECT_FALSE(interfaces->m_toSync.empty());
        EXPECT_EQ(gIntfsOrch->m_rehomeAdmissions.count("Ethernet0"), 0u);
        EXPECT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        std::string ownerId;
        EXPECT_FALSE(status->hget("Ethernet0", "owner_id", ownerId));

        interfaces->addToSync(rehomeRow("Ethernet0", "req-1", "prepare", "", "VrfRed", epoch));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        EXPECT_EQ(gIntfsOrch->m_rehomeAdmissions.count("Ethernet0"), 1u);
        ASSERT_TRUE(status->hget("Ethernet0", "owner_id", ownerId));
        EXPECT_EQ(ownerId, "req-1");
        EXPECT_EQ(gIntfsOrch->getRouterIntfsId("Ethernet0"), rif);
    }

    TEST_F(RehomeOwnerTest, RecoveryRestoresTheOldVrfWithTheLatestAddresses)
    {
        Table appIntfs(m_app_db.get(), APP_INTF_TABLE_NAME);
        appIntfs.set("Ethernet0", {{"vrf_name", "VrfBlue"}});
        appIntfs.set("Ethernet0:10.0.0.1/24", {{"family", "IPv4"}});
        gIntfsOrch->addExistingData(&appIntfs);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        const auto oldVrf = gVrfOrch->getVRFid("VrfBlue");
        const auto oldRif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(oldRif, SAI_NULL_OBJECT_ID);
        ASSERT_TRUE(interfaces->m_toSync.empty());

        openRehomeRequest(*status, "Ethernet0", "req-1", "VrfRed");
        interfaces->addToSync(rehomeRow("Ethernet0", "req-1", "prepare", "VrfBlue", "VrfRed", epoch));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        interfaces->addToSync(rehomeRow("Ethernet0", "req-1", "apply", "VrfRed", "VrfRed", epoch));
        fail_next_rif_create = true;
        ASSERT_NO_THROW(static_cast<Orch *>(gIntfsOrch)->doTask());
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().count("Ethernet0"), 0u);
        ASSERT_FALSE(interfaces->m_toSync.empty());
        EXPECT_TRUE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        appIntfs.del("Ethernet0:10.0.0.1/24");
        appIntfs.set("Ethernet0:10.1.0.1/24", {{"family", "IPv4"}});
        interfaces->addToSync(rehomeRow("Ethernet0", "req-2", "recover", "VrfBlue", "VrfRed", epoch));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        std::string ownerPhase;
        ASSERT_TRUE(status->hget("Ethernet0", "owner_phase", ownerPhase));
        EXPECT_EQ(ownerPhase, "fenced");
        EXPECT_EQ(gIntfsOrch->getRouterIntfsId("Ethernet0"), SAI_NULL_OBJECT_ID);

        interfaces->addToSync(rehomeRow("Ethernet0", "req-2", "restore", "VrfBlue", "VrfRed", epoch));
        interfaces->addToSync(KeyOpFieldsValuesTuple("Ethernet0:10.0.0.1/24", DEL_COMMAND, {}));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        EXPECT_TRUE(interfaces->m_toSync.empty());
        ASSERT_TRUE(status->hget("Ethernet0", "owner_phase", ownerPhase));
        EXPECT_EQ(ownerPhase, "restored");

        const auto restoredRif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(restoredRif, SAI_NULL_OBJECT_ID);
        EXPECT_NE(restoredRif, oldRif);
        const auto &applied = gIntfsOrch->getSyncdIntfses().at("Ethernet0");
        EXPECT_EQ(applied.vrf_id, oldVrf);
        EXPECT_EQ(applied.ip_addresses.count(IpPrefix("10.0.0.1/24")), 0u);
        EXPECT_EQ(applied.ip_addresses.count(IpPrefix("10.1.0.1/24")), 1u);
        sai_attribute_t attr{};
        attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
        ASSERT_EQ(sai_router_intfs_api->get_router_interface_attribute(restoredRif, 1, &attr), SAI_STATUS_SUCCESS);
        EXPECT_EQ(attr.value.oid, oldVrf);

        sai_route_entry_t route{};
        route.switch_id = gSwitchId;
        route.vr_id = oldVrf;
        attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        copy(route.destination, IpPrefix("10.0.0.1/32"));
        EXPECT_NE(sai_route_api->get_route_entry_attribute(&route, 1, &attr), SAI_STATUS_SUCCESS);
        copy(route.destination, IpPrefix("10.1.0.1/32"));
        EXPECT_EQ(sai_route_api->get_route_entry_attribute(&route, 1, &attr), SAI_STATUS_SUCCESS);
    }

    TEST_F(RehomeOwnerTest, BusyRemovalKeepsTheOldRifAndItsBookkeeping)
    {
        const auto oldRif = admitMoveToVrfRed("Ethernet0");
        ASSERT_FALSE(HasFatalFailure());
        const auto pendingCounters = gIntfsOrch->m_rifsToAdd.size();
        ASSERT_GT(pendingCounters, 0u);

        interfaces->addToSync(rehomeRow("Ethernet0", "req-1", "apply", "VrfRed", "VrfRed", epoch));
        fail_next_rif_remove = true;
        ASSERT_NO_THROW(static_cast<Orch *>(gIntfsOrch)->doTask());
        ASSERT_FALSE(interfaces->m_toSync.empty());
        EXPECT_EQ(gIntfsOrch->getRouterIntfsId("Ethernet0"), oldRif);
        EXPECT_EQ(gIntfsOrch->m_rifsToAdd.size(), pendingCounters);
        EXPECT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").vrf_id, gVirtualRouterId);

        NextHopGroupMember member(NextHopKey(IpAddress("0.0.0.0"), "Ethernet0"));
        EXPECT_EQ(member.getNhId(), SAI_NULL_OBJECT_ID);
        const auto id = sai_serialize_object_id(oldRif);
        if (gIntfsOrch->m_vidToRidTable)
        {
            gIntfsOrch->m_vidToRidTable->hset("", id, id);
        }
        gIntfsOrch->doTask(*gIntfsOrch->m_updateMapsTimer);
        std::string mappedRif;
        ASSERT_TRUE(gIntfsOrch->m_rifNameTable->hget("", "Ethernet0", mappedRif));
        EXPECT_EQ(mappedRif, id);

        static_cast<Orch *>(gIntfsOrch)->doTask();
        interfaces->addToSync(rehomeRow("Ethernet0", "req-1", "release", "VrfRed", "VrfRed", epoch));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        EXPECT_TRUE(interfaces->m_toSync.empty());
        EXPECT_NE(gIntfsOrch->getRouterIntfsId("Ethernet0"), oldRif);
        EXPECT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").vrf_id, gVrfOrch->getVRFid("VrfRed"));
        EXPECT_EQ(member.getNhId(), gIntfsOrch->getRouterIntfsId("Ethernet0"));
    }

    TEST_F(RehomeOwnerTest, ReferencedRifIsNeverRemovedAndWithdrawalUnblocksTheMove)
    {
        interfaces->addToSync(KeyOpFieldsValuesTuple("Ethernet0", SET_COMMAND, {{"vrf_name", ""}}));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        const auto oldRif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(oldRif, SAI_NULL_OBJECT_ID);

        auto routes = dynamic_cast<ConsumerBase *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(routes, nullptr);
        routes->addToSync(KeyOpFieldsValuesTuple("VrfBlue:10.20.0.0/24", SET_COMMAND,
            {{"ifname", "Ethernet0"}, {"nexthop", "0.0.0.0"}}));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_TRUE(routes->m_toSync.empty());
        sai_route_entry_t route{};
        route.switch_id = gSwitchId;
        route.vr_id = gVrfOrch->getVRFid("VrfBlue");
        copy(route.destination, IpPrefix("10.20.0.0/24"));
        sai_attribute_t attr{};
        attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        ASSERT_EQ(sai_route_api->get_route_entry_attribute(&route, 1, &attr), SAI_STATUS_SUCCESS);
        ASSERT_EQ(attr.value.oid, oldRif);

        openRehomeRequest(*status, "Ethernet0", "req-1", "VrfRed");
        interfaces->addToSync(rehomeRow("Ethernet0", "req-1", "prepare", "", "VrfRed", epoch));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        std::string ownerPhase, refCount;
        ASSERT_TRUE(status->hget("Ethernet0", "owner_phase", ownerPhase));
        ASSERT_TRUE(status->hget("Ethernet0", "ref_count", refCount));
        EXPECT_EQ(ownerPhase, "waiting");
        EXPECT_EQ(refCount, "1");

        interfaces->addToSync(rehomeRow("Ethernet0", "req-1", "apply", "VrfRed", "VrfRed", epoch));
        for (unsigned int pass = 0; pass < 3; ++pass)
        {
            static_cast<Orch *>(gIntfsOrch)->doTask();
        }
        EXPECT_FALSE(interfaces->m_toSync.empty());
        EXPECT_EQ(gIntfsOrch->getRouterIntfsId("Ethernet0"), oldRif);
        ASSERT_EQ(sai_route_api->get_route_entry_attribute(&route, 1, &attr), SAI_STATUS_SUCCESS);
        EXPECT_EQ(attr.value.oid, oldRif);

        routes->addToSync(KeyOpFieldsValuesTuple("VrfBlue:10.20.0.0/24", DEL_COMMAND, {}));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_NE(sai_route_api->get_route_entry_attribute(&route, 1, &attr), SAI_STATUS_SUCCESS);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        interfaces->addToSync(rehomeRow("Ethernet0", "req-1", "release", "VrfRed", "VrfRed", epoch));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        EXPECT_TRUE(interfaces->m_toSync.empty());
        const auto newRif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(newRif, oldRif);
        routes->addToSync(KeyOpFieldsValuesTuple("VrfBlue:10.20.0.0/24", SET_COMMAND,
            {{"ifname", "Ethernet0"}, {"nexthop", "0.0.0.0"}}));
        static_cast<Orch *>(gRouteOrch)->doTask();
        attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        ASSERT_EQ(sai_route_api->get_route_entry_attribute(&route, 1, &attr), SAI_STATUS_SUCCESS);
        EXPECT_EQ(attr.value.oid, newRif);
    }

    TEST_F(RehomeOwnerTest, AdmissionDefersLabelRouteAcquisitionButNotWithdrawal)
    {
        const auto oldRif = admitMoveToVrfRed("Ethernet0");
        ASSERT_FALSE(HasFatalFailure());

        auto labels = dynamic_cast<ConsumerBase *>(gRouteOrch->getExecutor(APP_LABEL_ROUTE_TABLE_NAME));
        ASSERT_NE(labels, nullptr);
        labels->addToSync(KeyOpFieldsValuesTuple("100", SET_COMMAND,
            {{"nexthop", "0.0.0.0"}, {"ifname", "Ethernet0"}, {"mpls_pop", "1"}}));
        labels->addToSync(KeyOpFieldsValuesTuple("200", SET_COMMAND,
            {{"nexthop", "0.0.0.0"}, {"ifname", "Ethernet0"}, {"mpls_pop", "1"}}));
        static_cast<Orch *>(gRouteOrch)->doTask();
        EXPECT_EQ(labels->m_toSync.count("100"), 1u);
        EXPECT_EQ(labels->m_toSync.count("200"), 1u);
        sai_inseg_entry_t inseg{};
        inseg.switch_id = gSwitchId;
        inseg.label = 100;
        sai_attribute_t attr{};
        attr.id = SAI_INSEG_ENTRY_ATTR_NEXT_HOP_ID;
        EXPECT_NE(sai_mpls_api->get_inseg_entry_attribute(&inseg, 1, &attr), SAI_STATUS_SUCCESS);

        labels->addToSync(KeyOpFieldsValuesTuple("200", DEL_COMMAND, {}));
        static_cast<Orch *>(gRouteOrch)->doTask();
        EXPECT_EQ(labels->m_toSync.count("200"), 0u);

        applyAndRelease("Ethernet0");
        const auto newRif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(newRif, oldRif);
        static_cast<Orch *>(gRouteOrch)->doTask();
        EXPECT_TRUE(labels->m_toSync.empty());
        ASSERT_EQ(sai_mpls_api->get_inseg_entry_attribute(&inseg, 1, &attr), SAI_STATUS_SUCCESS);
        EXPECT_EQ(attr.value.oid, newRif);
        inseg.label = 200;
        EXPECT_NE(sai_mpls_api->get_inseg_entry_attribute(&inseg, 1, &attr), SAI_STATUS_SUCCESS);
    }

    TEST_F(RehomeOwnerTest, AdmissionDefersFineGrainedRifFallback)
    {
        auto groups = dynamic_cast<Consumer *>(gFgNhgOrch->getExecutor(CFG_FG_NHG));
        ASSERT_NE(groups, nullptr);
        groups->addToSync(KeyOpFieldsValuesTuple("fg-rehome", SET_COMMAND,
            {{"bucket_size", "4"}, {"match_mode", "prefix-based"}, {"max_next_hops", "1"}}));
        static_cast<Orch *>(gFgNhgOrch)->doTask();
        auto prefixes = dynamic_cast<Consumer *>(gFgNhgOrch->getExecutor(CFG_FG_NHG_PREFIX));
        ASSERT_NE(prefixes, nullptr);
        prefixes->addToSync(KeyOpFieldsValuesTuple("198.51.100.0/24", SET_COMMAND, {{"FG_NHG", "fg-rehome"}}));
        static_cast<Orch *>(gFgNhgOrch)->doTask();
        ASSERT_TRUE(prefixes->m_toSync.empty());

        const auto oldRif = admitMoveToVrfRed("Ethernet0");
        ASSERT_FALSE(HasFatalFailure());

        NextHopGroupKey nextHops;
        nextHops.add(NextHopKey(IpAddress("10.0.0.2"), "Ethernet0"));
        const IpPrefix prefix("198.51.100.0/24");
        sai_object_id_t nextHop = SAI_NULL_OBJECT_ID;
        bool changed = false;
        ASSERT_FALSE(gFgNhgOrch->setFgNhg(gVirtualRouterId, prefix, nextHops, nextHop, changed));
        EXPECT_FALSE(gFgNhgOrch->syncdContainsFgNhg(gVirtualRouterId, prefix));

        applyAndRelease("Ethernet0");
        const auto newRif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(newRif, oldRif);
        ASSERT_TRUE(gFgNhgOrch->setFgNhg(gVirtualRouterId, prefix, nextHops, nextHop, changed));
        EXPECT_TRUE(changed);
        EXPECT_EQ(nextHop, newRif);
        EXPECT_TRUE(gFgNhgOrch->syncdContainsFgNhg(gVirtualRouterId, prefix));
    }

    TEST_F(RehomeOwnerTest, DeletionSupersedesTheRequestAndRetiresItsBookkeeping)
    {
        interfaces->addToSync(KeyOpFieldsValuesTuple("Ethernet0", SET_COMMAND, {{"vrf_name", "VrfBlue"}}));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_NE(gIntfsOrch->getRouterIntfsId("Ethernet0"), SAI_NULL_OBJECT_ID);

        openRehomeRequest(*status, "Ethernet0", "req-1", "VrfRed");
        interfaces->addToSync(rehomeRow("Ethernet0", "req-1", "prepare", "VrfBlue", "VrfRed", epoch));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        interfaces->addToSync(rehomeRow("Ethernet0", "req-1", "apply", "VrfRed", "VrfRed", epoch));
        fail_next_rif_create = true;
        ASSERT_NO_THROW(static_cast<Orch *>(gIntfsOrch)->doTask());
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().count("Ethernet0"), 0u);
        ASSERT_EQ(gIntfsOrch->m_rehomeIntfses.count("Ethernet0"), 1u);

        interfaces->addToSync(KeyOpFieldsValuesTuple("Ethernet0", DEL_COMMAND, {}));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        EXPECT_TRUE(interfaces->m_toSync.empty());
        EXPECT_EQ(gIntfsOrch->m_rehomeAdmissions.count("Ethernet0"), 0u);
        EXPECT_EQ(gIntfsOrch->m_rehomeIntfses.count("Ethernet0"), 0u);
        EXPECT_EQ(gIntfsOrch->m_removingIntfses.count("Ethernet0"), 0u);
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        EXPECT_EQ(gPortsOrch->m_port_ref_count["Ethernet0"], 0u);
        std::string value;
        EXPECT_TRUE(status->hget("Ethernet0", "manager_id", value));
        EXPECT_TRUE(status->hget("Ethernet0", "owner_id", value));

        interfaces->addToSync(KeyOpFieldsValuesTuple("Ethernet0", SET_COMMAND, {{"vrf_name", "VrfBlue"}}));
        fail_next_rif_create = true;
        EXPECT_THROW(gIntfsOrch->doTask(*interfaces), std::runtime_error);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_NE(gIntfsOrch->getRouterIntfsId("Ethernet0"), SAI_NULL_OBJECT_ID);

        openRehomeRequest(*status, "Ethernet0", "req-2", "VrfRed");
        interfaces->addToSync(rehomeRow("Ethernet0", "req-2", "prepare", "VrfBlue", "VrfRed", epoch));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(gIntfsOrch->m_rehomeAdmissions.count("Ethernet0"), 1u);
        ASSERT_TRUE(status->hget("Ethernet0", "owner_id", value));
        const auto blueRif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        status->del("Ethernet0");
        interfaces->addToSync(KeyOpFieldsValuesTuple("Ethernet0", SET_COMMAND,
            {{"vrf_name", "VrfBlue"}, {"rehome_id", ""}, {"rehome_phase", ""},
             {"rehome_target", ""}, {"rehome_epoch", ""}}));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        EXPECT_TRUE(interfaces->m_toSync.empty());
        EXPECT_EQ(gIntfsOrch->m_rehomeAdmissions.count("Ethernet0"), 0u);
        EXPECT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        EXPECT_EQ(gIntfsOrch->getRouterIntfsId("Ethernet0"), blueRif);
        EXPECT_FALSE(status->hget("Ethernet0", "owner_id", value));
    }

    struct SubPortRehomeTest : RehomeOwnerTest
    {
        uint32_t parentRefBefore = 0;

        void SetUp() override
        {
            RehomeOwnerTest::SetUp();
            parentRefBefore = gPortsOrch->m_port_ref_count["Ethernet0"];
        }
    };

    TEST_F(SubPortRehomeTest, RootDeleteReclaimsTheSubPortAFailedTargetRetained)
    {
        interfaces->addToSync(KeyOpFieldsValuesTuple("Ethernet0.10", SET_COMMAND,
            {{"vlan", "10"}, {"admin_status", "up"}, {"vrf_name", ""}}));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(interfaces->m_toSync.empty());
        Port subPort;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0.10", subPort));
        ASSERT_EQ(subPort.m_type, Port::SUBPORT);
        ASSERT_NE(gIntfsOrch->getRouterIntfsId("Ethernet0.10"), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(hostif_keep_tag_count, 1);

        openRehomeRequest(*status, "Ethernet0.10", "req-1", "VrfRed");
        interfaces->addToSync(rehomeRow("Ethernet0.10", "req-1", "prepare", "", "VrfRed", epoch));
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(gIntfsOrch->m_rehomeAdmissions.count("Ethernet0.10"), 1u);

        interfaces->addToSync(rehomeRow("Ethernet0.10", "req-1", "apply", "VrfRed", "VrfRed", epoch));
        fail_next_rif_create = true;
        ASSERT_NO_THROW(static_cast<Orch *>(gIntfsOrch)->doTask());
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().count("Ethernet0.10"), 0u);
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0.10", subPort));
        Port parentDuring;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", parentDuring));
        ASSERT_EQ(parentDuring.m_child_ports.count("Ethernet0.10"), 1u);
        ASSERT_GT(gPortsOrch->m_port_ref_count["Ethernet0"], parentRefBefore);

        interfaces->addToSync(KeyOpFieldsValuesTuple("Ethernet0.10", DEL_COMMAND, {}));
        fail_next_hostif_vlan_tag_set = true;
        ASSERT_NO_THROW(static_cast<Orch *>(gIntfsOrch)->doTask());
        EXPECT_FALSE(interfaces->m_toSync.empty());
        EXPECT_EQ(hostif_strip_tag_count, 0);
        EXPECT_TRUE(gPortsOrch->getPort("Ethernet0.10", subPort));
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", parentDuring));
        EXPECT_EQ(parentDuring.m_child_ports.count("Ethernet0.10"), 1u);

        static_cast<Orch *>(gIntfsOrch)->doTask();
        EXPECT_TRUE(interfaces->m_toSync.empty());
        Port removed;
        EXPECT_FALSE(gPortsOrch->getPort("Ethernet0.10", removed));
        EXPECT_EQ(gPortsOrch->m_portList.count("Ethernet0.10"), 0u);
        Port parentAfter;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", parentAfter));
        EXPECT_EQ(parentAfter.m_child_ports.count("Ethernet0.10"), 0u);
        EXPECT_EQ(gPortsOrch->m_port_ref_count["Ethernet0"], parentRefBefore);
        EXPECT_EQ(hostif_strip_tag_count, 1);
        EXPECT_EQ(gIntfsOrch->m_rehomeAdmissions.count("Ethernet0.10"), 0u);
    }

    TEST_F(IntfsOrchTest, IntfsOrchDeleteCreateRetry)
    {
        // create a interface
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "SET", { {"mtu", "9100"}}});
        auto consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        auto current_create_count = create_rif_count;
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(current_create_count + 1, create_rif_count);

        // create dependency to the interface
        gIntfsOrch->increaseRouterIntfsRefCount("Ethernet0");

        // delete the interface, expect retry because dependency exists
        entries.clear();
        entries.push_back({"Ethernet0", "DEL", { {} }});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        auto current_remove_count = remove_rif_count;
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(current_remove_count, remove_rif_count);

        // create the interface again, expect retry because interface is in removing
        entries.clear();
        entries.push_back({"Ethernet0", "SET", { {"mtu", "9100"}}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        current_create_count = create_rif_count;
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(current_create_count, create_rif_count);

        // remove the dependency, expect delete and create a new one
        gIntfsOrch->decreaseRouterIntfsRefCount("Ethernet0");
        current_create_count = create_rif_count;
        current_remove_count = remove_rif_count;
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(current_create_count + 1, create_rif_count);
        ASSERT_EQ(current_remove_count + 1, remove_rif_count);
    };

    TEST_F(IntfsOrchTest, IntfsOrchVrfUpdate)
    {
        //create a new vrf
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Vrf-Blue", "SET", { {"NULL", "NULL"}}});
        auto consumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gVrfOrch)->doTask(); 
        ASSERT_TRUE(gVrfOrch->isVRFexists("Vrf-Blue"));
        auto new_vrf_reference_count = gVrfOrch->getVrfRefCount("Vrf-Blue");
        ASSERT_EQ(new_vrf_reference_count, 0);

        // create an interface
        entries.clear();
        entries.push_back({"Loopback2", "SET", {}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        IntfsTable m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(m_syncdIntfses["Loopback2"].vrf_id, gVirtualRouterId);

        // change vrf and check if it worked
        entries.clear();
        entries.push_back({"Loopback2", "SET", { {"vrf_name", "Vrf-Blue"}}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        auto new_vrf_updated_reference_count = gVrfOrch->getVrfRefCount("Vrf-Blue");
        ASSERT_EQ(new_vrf_reference_count + 1, new_vrf_updated_reference_count);
        m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(m_syncdIntfses["Loopback2"].vrf_id, gVrfOrch->getVRFid("Vrf-Blue"));

        // create an interface
        entries.clear();
        entries.push_back({"Loopback3", "SET", {}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(m_syncdIntfses["Loopback3"].vrf_id, gVirtualRouterId);

        // Add IP address to the interface
        entries.clear();
        entries.push_back({"Loopback3:3.3.3.3/32", "SET", {{"scope", "global"},{"family", "IPv4"}}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        // change vrf and check it doesn't affect the interface due to existing IP
        entries.clear();
        entries.push_back({"Loopback3", "SET", { {"vrf_name", "Vrf-Blue"}}});
        consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        m_syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(m_syncdIntfses["Loopback3"].vrf_id, gVirtualRouterId);    
    }

    TEST_F(IntfsOrchTest, IntfsOrchSagEnableDisable)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;

        entries.push_back({"GLOBAL", "SET", {{"gateway_mac", "02:03:04:05:06:07"}}});
        auto sagConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_SAG_TABLE_NAME));
        ASSERT_NE(sagConsumer, nullptr);
        sagConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        entries.clear();
        entries.push_back({"Ethernet0", "SET", {{"mtu", "9100"}, {"static_anycast_gateway", "true"}}});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        ASSERT_NE(intfConsumer, nullptr);
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        auto syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_NE(syncdIntfses.find("Ethernet0"), syncdIntfses.end());
        EXPECT_TRUE(syncdIntfses.at("Ethernet0").sag_enabled);

        entries.clear();
        entries.push_back({"Ethernet0", "SET", {{"static_anycast_gateway", "false"}}});
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        syncdIntfses = gIntfsOrch->getSyncdIntfses();
        ASSERT_NE(syncdIntfses.find("Ethernet0"), syncdIntfses.end());
        EXPECT_FALSE(syncdIntfses.at("Ethernet0").sag_enabled);

        entries.clear();
        entries.push_back({"GLOBAL", "DEL", {}});
        sagConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
    }

    // Regression test for the batched IP-removal + VRF-bind race.
    // m_toSync is an ordered multimap, so within a single drain the bare interface
    // key ("Loopback6") is processed before the per-IP key ("Loopback6:6.6.6.6/32").
    // When a config sequence removes the loopback IPs and rebinds the interface to a
    // VRF back-to-back, both land in one batch and the VRF-change SET is evaluated
    // while the IP is still present. The fix defers (retains) that SET instead of
    // logging an error and dropping it, so the bind converges on a later drain.
    TEST_F(IntfsOrchTest, IntfsOrchVrfBindDeferredUntilIpRemoved)
    {
        // create a new vrf
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Vrf-Blue", "SET", { {"NULL", "NULL"}}});
        auto vrfConsumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        vrfConsumer->addToSync(entries);
        static_cast<Orch *>(gVrfOrch)->doTask();
        ASSERT_TRUE(gVrfOrch->isVRFexists("Vrf-Blue"));
        auto base_vrf_ref = gVrfOrch->getVrfRefCount("Vrf-Blue");

        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));

        // create a loopback in the default vrf and give it an IP address
        entries.clear();
        entries.push_back({"Loopback6", "SET", {}});
        entries.push_back({"Loopback6:6.6.6.6/32", "SET", {{"scope", "global"}, {"family", "IPv4"}}});
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        auto syncd = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(syncd["Loopback6"].vrf_id, gVirtualRouterId);
        ASSERT_EQ(syncd["Loopback6"].ip_addresses.size(), static_cast<size_t>(1));

        // Single batch: remove the IP AND rebind the interface to Vrf-Blue.
        // The bare "Loopback6" SET sorts before "Loopback6:6.6.6.6/32" DEL, so the
        // bind is evaluated first (IP still present) and must be deferred, not dropped.
        entries.clear();
        entries.push_back({"Loopback6", "SET", { {"vrf_name", "Vrf-Blue"}}});
        entries.push_back({"Loopback6:6.6.6.6/32", "DEL", {}});
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        // After the first drain: IP removed, interface still present and still in the
        // default vrf (the bind is pending, not lost).
        syncd = gIntfsOrch->getSyncdIntfses();
        ASSERT_NE(syncd.find("Loopback6"), syncd.end());
        ASSERT_EQ(syncd["Loopback6"].ip_addresses.size(), static_cast<size_t>(0));
        ASSERT_EQ(syncd["Loopback6"].vrf_id, gVirtualRouterId);

        // The deferred bind is retried on the next drain and now succeeds.
        static_cast<Orch *>(gIntfsOrch)->doTask();
        syncd = gIntfsOrch->getSyncdIntfses();
        ASSERT_EQ(syncd["Loopback6"].vrf_id, gVrfOrch->getVRFid("Vrf-Blue"));
        ASSERT_EQ(gVrfOrch->getVrfRefCount("Vrf-Blue"), base_vrf_ref + 1);
    }

    TEST_F(IntfsOrchTest, IntfsOrchRetriesLoopbackActionSetFailure)
    {
        std::deque<KeyOpFieldsValuesTuple> entries{
            {"Ethernet0", "SET", {{"mtu", "9100"}}}
        };
        auto consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        fail_next_rif_set = true;
        entries = {
            {"Ethernet0", "SET", {{"loopback_action", "drop"}}}
        };
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_EQ(consumer->m_toSync.size(), 1u);
        ASSERT_EQ(loopback_action_set_count, 1);
        ASSERT_FALSE(saw_loopback_action);

        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(loopback_action_set_count, 2);
        ASSERT_TRUE(saw_loopback_action);
        ASSERT_EQ(last_loopback_action, SAI_PACKET_ACTION_DROP);
    }

    TEST_F(IntfsOrchTest, IntfsOrchIgnoresInvalidLoopbackActionField)
    {
        std::deque<KeyOpFieldsValuesTuple> entries{
            {"Ethernet0", "SET", {{"mtu", "9100"}}}
        };
        auto consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        entries = {
            {"Ethernet0", "SET", {
                {"loopback_action", "invalid"},
                {"nat_zone", "7"}
            }}
        };
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(loopback_action_set_count, 0);
        ASSERT_FALSE(saw_loopback_action);

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_EQ(port.m_nat_zone_id, 7u);
    }
}
