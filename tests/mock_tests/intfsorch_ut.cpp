#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#include "gtest/gtest.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include <memory>
#include <vector>



namespace intfsorch_test
{
    using namespace std;

    int create_rif_count = 0;
    int remove_rif_count = 0;
    sai_router_interface_api_t *pold_sai_rif_api;
    sai_router_interface_api_t ut_sai_rif_api;

    int remove_route_entries_count = 0;
    bool fail_remove_route_entries = false;
    sai_route_api_t *pold_sai_route_api;
    sai_route_api_t ut_sai_route_api;

    sai_status_t _ut_create_router_interface(
            _Out_ sai_object_id_t *router_interface_id,
            _In_ sai_object_id_t switch_id,
            _In_ uint32_t attr_count,
            _In_ const sai_attribute_t *attr_list)
    {
        ++create_rif_count;
        return pold_sai_rif_api->create_router_interface(router_interface_id, switch_id, attr_count, attr_list);
    }

    sai_status_t _ut_remove_router_interface(
            _In_ sai_object_id_t router_interface_id)
    {
        ++remove_rif_count;
        return pold_sai_rif_api->remove_router_interface(router_interface_id);
    }

    sai_status_t _ut_remove_route_entries(
            _In_ uint32_t object_count,
            _In_ const sai_route_entry_t *route_entry,
            _In_ sai_bulk_op_error_mode_t mode,
            _Out_ sai_status_t *object_statuses)
    {
        if (fail_remove_route_entries)
        {
            for (uint32_t i = 0; i < object_count; i++)
            {
                object_statuses[i] = SAI_STATUS_FAILURE;
            }
            return SAI_STATUS_FAILURE;
        }
        remove_route_entries_count += object_count;
        return pold_sai_route_api->remove_route_entries(object_count, route_entry, mode, object_statuses);
    }

    int create_route_entries_count = 0;
    sai_object_id_t last_created_route_nexthop = SAI_NULL_OBJECT_ID;

    sai_status_t _ut_create_route_entries(
            _In_ uint32_t object_count,
            _In_ const sai_route_entry_t *route_entry,
            _In_ const uint32_t *attr_count,
            _In_ const sai_attribute_t **attr_list,
            _In_ sai_bulk_op_error_mode_t mode,
            _Out_ sai_status_t *object_statuses)
    {
        create_route_entries_count += object_count;
        for (uint32_t i = 0; i < object_count; i++)
        {
            for (uint32_t j = 0; j < attr_count[i]; j++)
            {
                if (attr_list[i][j].id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
                {
                    last_created_route_nexthop = attr_list[i][j].value.oid;
                }
            }
        }
        return pold_sai_route_api->create_route_entries(object_count, route_entry, attr_count, attr_list, mode, object_statuses);
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
            testing_db::reset();

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

            pold_sai_route_api = sai_route_api;
            ut_sai_route_api = *sai_route_api;
            sai_route_api = &ut_sai_route_api;

            sai_route_api->remove_route_entries = _ut_remove_route_entries;
            remove_route_entries_count = 0;
            fail_remove_route_entries = false;

            sai_route_api->create_route_entries = _ut_create_route_entries;
            create_route_entries_count = 0;
            last_created_route_nexthop = SAI_NULL_OBJECT_ID;

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
            ASSERT_EQ(gTunneldecapOrch, nullptr);
            gTunneldecapOrch = new TunnelDecapOrch(m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);
            vector<string> mux_tables = {
                CFG_MUX_CABLE_TABLE_NAME,
                CFG_PEER_SWITCH_TABLE_NAME
            };
            auto* mux_orch = new MuxOrch(m_config_db.get(), mux_tables, gTunneldecapOrch, gNeighOrch, gFdbOrch);
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

            delete gNhgOrch;
            gNhgOrch = nullptr;

            delete gBufferOrch;
            gBufferOrch = nullptr;

            delete gTunneldecapOrch;
            gTunneldecapOrch = nullptr;

            delete gVrfOrch;
            gVrfOrch = nullptr;

            delete gFlowCounterRouteOrch;
            gFlowCounterRouteOrch = nullptr;

            sai_router_intfs_api = pold_sai_rif_api;
            sai_route_api = pold_sai_route_api;
            ut_helper::uninitSaiApi();

            testing_db::reset();
        }
    };

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

    // Regression tests for the VRF-rehome route/RIF race (route programmed
    // against a RIF whose removal is pending). When an interface removal is
    // blocked by route references, IntfsOrch asks RouteOrch to evict the
    // pinning routes and park their APPL_DB intent in the retry cache; the
    // parked intent replays after the interface is recreated. Without the
    // repair the stale route wedges the removal indefinitely.

    static void createIntf(const std::string& alias)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({alias, "SET", { {"mtu", "9100"}}});
        auto consumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_NE(gIntfsOrch->getRouterIntfsId(alias), SAI_NULL_OBJECT_ID);
    }

    static void programIntfRoute(const std::string& key, const std::string& alias, swss::DBConnector *appDb)
    {
        std::vector<FieldValueTuple> fvs = {
            {"nexthop", "0.0.0.0"}, {"ifname", alias}, {"protocol", "static"}};
        // The APPL_DB main hash holds the popped route intent
        Table appRouteTable(appDb, APP_ROUTE_TABLE_NAME);
        appRouteTable.set(key, fvs);
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({key, "SET", fvs});
        auto consumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gRouteOrch)->doTask();
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictRepairsRouteHeldWedge)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        sai_object_id_t old_rif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.1.0/24", "Ethernet0", m_app_db.get()));
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.1.0/24")));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);

        // Interface removal is blocked by the route: the route is evicted,
        // its reference released and its intent parked for replay
        auto base_remove_rif = remove_rif_count;
        auto base_remove_route = remove_route_entries_count;
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_TRUE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.1.0/24")));
        ASSERT_EQ(base_remove_route + 1, remove_route_entries_count);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 0);
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_NE(rc, nullptr);
        ASSERT_EQ(rc->getRetryMap().size(), 1u);
        ASSERT_EQ(rc->getRetryMap().begin()->second.first, make_constraint(RETRY_CST_INTF, "Ethernet0"));

        // The retained DEL completes on the next drain
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_EQ(base_remove_rif + 1, remove_rif_count);
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        // Recreating the interface resolves the constraint and the parked
        // intent replays against the new RIF at the SAI level
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        sai_object_id_t new_rif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(new_rif, old_rif);
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.1.0/24")));
        ASSERT_EQ(last_created_route_nexthop, new_rif);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);
        ASSERT_TRUE(rc->getRetryMap().empty());

        // Replay is one-shot: another drain changes nothing
        auto base_create_route = create_route_entries_count;
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_EQ(base_create_route, create_route_entries_count);
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.1.0/24")));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictSkipsRouteWithDeletedMainHash)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.2.0/24", "Ethernet0", m_app_db.get()));

        // Simulate a route DEL already popped: the pop deletes the main hash
        Table appRouteTable(m_app_db.get(), APP_ROUTE_TABLE_NAME);
        appRouteTable.del("10.10.2.0/24");

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        // The route is evicted but nothing is parked: replaying it would
        // resurrect a route that is being deleted
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.2.0/24")));
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_TRUE(rc->getRetryMap().empty());

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.2.0/24")));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 0);
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictSkipsRouteWithPendingDel)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.3.0/24", "Ethernet0", m_app_db.get()));

        // A DEL for the route is popped into m_toSync but not yet processed;
        // the main hash may still be present at this point
        auto routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> routeEntries;
        routeEntries.push_back({"10.10.3.0/24", "DEL", {}});
        routeConsumer->addToSync(routeEntries);

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        // The route is evicted but nothing is parked behind the pending DEL
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.3.0/24")));
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_TRUE(rc->getRetryMap().empty());

        // The pending DEL processes as a no-op and the removal completes
        static_cast<Orch *>(gRouteOrch)->doTask();
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.3.0/24")));
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictSkipsMixedHolderWedge)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.4.0/24", "Ethernet0", m_app_db.get()));

        // A non-route reference on the same interface keeps the removal
        // blocked whatever the eviction does, so evicting the route could not
        // complete the removal and would strand it with no replay event
        gIntfsOrch->increaseRouterIntfsRefCount("Ethernet0");

        auto base_remove_route = remove_route_entries_count;
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        static_cast<Orch *>(gIntfsOrch)->doTask();
        static_cast<Orch *>(gIntfsOrch)->doTask();

        // Behavior is exactly today's: the route keeps forwarding and the
        // wedge persists, with no repeated scanning
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.4.0/24")));
        ASSERT_EQ(base_remove_route, remove_route_entries_count);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 2);
        ASSERT_TRUE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_TRUE(rc->getRetryMap().empty());

        // Once the other holder releases, the reference count change is
        // re-evaluated and the now repairable wedge is evicted exactly once
        gIntfsOrch->decreaseRouterIntfsRefCount("Ethernet0");
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.4.0/24")));
        ASSERT_EQ(base_remove_route + 1, remove_route_entries_count);
        ASSERT_EQ(rc->getRetryMap().size(), 1u);

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.4.0/24")));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);
        ASSERT_TRUE(rc->getRetryMap().empty());
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictReleasesParkedIntentWhenWedgeBecomesUnrepairable)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.11.0/24", "Ethernet0", m_app_db.get()));

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_EQ(rc->getRetryMap().size(), 1u);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 0);

        // A non-route holder takes a reference before the removal completes:
        // the interface will never be recreated, so the parked intent must be
        // released back to the consumer queue instead of waiting forever
        gIntfsOrch->increaseRouterIntfsRefCount("Ethernet0");
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        ASSERT_TRUE(rc->getRetryMap().empty());

        // The released task is retained by the interface guard, not lost, and
        // programs once the interface comes back
        auto routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_EQ(routeConsumer->m_toSync.count("10.10.11.0/24"), 1u);
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.11.0/24")));

        gIntfsOrch->decreaseRouterIntfsRefCount("Ethernet0");
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.11.0/24")));
    }

    TEST_F(IntfsOrchTest, IntfsOrchNoEvictionWhileAddressesRemain)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0:10.88.0.1/24", "SET", { {"scope", "global"}, {"family", "IPv4"} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.10.0/24", "Ethernet0", m_app_db.get()));

        // The whole interface DEL is tried before the address DEL and fails
        // because addresses remain, not because of references: that failure
        // must not evict anything
        auto base_remove_route = remove_route_entries_count;
        entries.clear();
        entries.push_back({"Ethernet0", "DEL", { {} }});
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        ASSERT_EQ(base_remove_route, remove_route_entries_count);
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.10.0/24")));
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_TRUE(rc->getRetryMap().empty());

        // Once the address is gone the removal reaches the reference check and
        // the route wedge is repaired
        entries.clear();
        entries.push_back({"Ethernet0:10.88.0.1/24", "DEL", {}});
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.10.0/24")));
        ASSERT_EQ(base_remove_route + 1, remove_route_entries_count);
        ASSERT_EQ(rc->getRetryMap().size(), 1u);

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictSkipsStaleBlackholeIntent)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.12.0/24", "Ethernet0", m_app_db.get()));

        // The main hash is a field union: a blackhole field left over from an
        // earlier shape of the prefix must not be replayed as the intent
        Table appRouteTable(m_app_db.get(), APP_ROUTE_TABLE_NAME);
        appRouteTable.set("10.10.12.0/24", { {"nexthop", "0.0.0.0"}, {"ifname", "Ethernet0"},
                                             {"protocol", "static"}, {"blackhole", "true"} });

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.12.0/24")));
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_TRUE(rc->getRetryMap().empty());

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.12.0/24")));
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictReplaysAfterVrfRebind)
    {
        // The issue 28619 sequence end to end: an interface is unbound from a
        // VRF, a racing route pins the old RIF, and the interface is recreated
        // in a different VRF
        std::deque<KeyOpFieldsValuesTuple> vrfEntries;
        vrfEntries.push_back({"Vrf-Blue", "SET", { {"NULL", "NULL"}}});
        auto vrfConsumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        vrfConsumer->addToSync(vrfEntries);
        static_cast<Orch *>(gVrfOrch)->doTask();
        ASSERT_TRUE(gVrfOrch->isVRFexists("Vrf-Blue"));

        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        sai_object_id_t old_rif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.13.0/24", "Ethernet0", m_app_db.get()));

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.13.0/24")));

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        // Rebind into Vrf-Blue: the RIF is recreated in the new VRF and the
        // parked intent replays against it
        entries.clear();
        entries.push_back({"Ethernet0", "SET", { {"vrf_name", "Vrf-Blue"}, {"mtu", "9100"} }});
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        sai_object_id_t new_rif = gIntfsOrch->getRouterIntfsId("Ethernet0");
        ASSERT_NE(new_rif, SAI_NULL_OBJECT_ID);
        ASSERT_NE(new_rif, old_rif);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").vrf_id, gVrfOrch->getVRFid("Vrf-Blue"));

        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.13.0/24")));
        ASSERT_EQ(last_created_route_nexthop, new_rif);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);
        ASSERT_TRUE(gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME)->getRetryMap().empty());
    }

    TEST_F(IntfsOrchTest, IntfsOrchNoRouteEvictionForNonRouteWedge)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet4"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.5.0/24", "Ethernet4", m_app_db.get()));

        // Ethernet0 is blocked by a non-route reference: no route eviction
        // may happen anywhere
        gIntfsOrch->increaseRouterIntfsRefCount("Ethernet0");

        auto base_remove_route = remove_route_entries_count;
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_TRUE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        ASSERT_EQ(base_remove_route, remove_route_entries_count);
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.5.0/24")));
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_TRUE(rc->getRetryMap().empty());

        // Same behavior as before the fix: the removal completes once the
        // reference is released
        gIntfsOrch->decreaseRouterIntfsRefCount("Ethernet0");
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.5.0/24")));
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictReplaysLeakedVrfRoute)
    {
        // Route leaked into another VRF with an interface next hop: it must
        // program normally, and an eviction episode must replay it unchanged
        std::deque<KeyOpFieldsValuesTuple> vrfEntries;
        vrfEntries.push_back({"Vrf-Blue", "SET", { {"NULL", "NULL"}}});
        auto vrfConsumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        vrfConsumer->addToSync(vrfEntries);
        static_cast<Orch *>(gVrfOrch)->doTask();
        ASSERT_TRUE(gVrfOrch->isVRFexists("Vrf-Blue"));
        sai_object_id_t blue_vrf_id = gVrfOrch->getVRFid("Vrf-Blue");
        auto base_vrf_ref = gVrfOrch->getVrfRefCount("Vrf-Blue");

        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("Vrf-Blue:10.55.0.0/24", "Ethernet0", m_app_db.get()));

        // Nothing blocks a cross-VRF interface route outside a removal episode
        ASSERT_TRUE(gRouteOrch->isRouteExists(blue_vrf_id, IpPrefix("10.55.0.0/24")));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_FALSE(gRouteOrch->isRouteExists(blue_vrf_id, IpPrefix("10.55.0.0/24")));
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_EQ(rc->getRetryMap().size(), 1u);
        ASSERT_EQ(rc->getRetryMap().begin()->first, "Vrf-Blue:10.55.0.0/24");

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_TRUE(gRouteOrch->isRouteExists(blue_vrf_id, IpPrefix("10.55.0.0/24")));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);
        ASSERT_EQ(gVrfOrch->getVrfRefCount("Vrf-Blue"), base_vrf_ref + 1);
        ASSERT_TRUE(gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME)->getRetryMap().empty());
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictDefaultRouteEpisode)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("0.0.0.0/0", "Ethernet0", m_app_db.get()));
        ASSERT_EQ(gRouteOrch->getSyncdRouteNhgKey(gVirtualRouterId, IpPrefix("0.0.0.0/0")).getSize(), 1u);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);

        // The default route is neutered rather than removed, but the
        // reference is released and the intent parked all the same
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_EQ(gRouteOrch->getSyncdRouteNhgKey(gVirtualRouterId, IpPrefix("0.0.0.0/0")).getSize(), 0u);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 0);
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_EQ(rc->getRetryMap().size(), 1u);

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_EQ(gRouteOrch->getSyncdRouteNhgKey(gVirtualRouterId, IpPrefix("0.0.0.0/0")).getSize(), 1u);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);
        ASSERT_TRUE(gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME)->getRetryMap().empty());
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictSaiFailureIsFailSafe)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.6.0/24", "Ethernet0", m_app_db.get()));

        // The eviction SAI removal fails: the route keeps its reference,
        // nothing is parked and the wedge persists exactly as before the fix
        fail_remove_route_entries = true;
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        fail_remove_route_entries = false;

        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.6.0/24")));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);
        ASSERT_TRUE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_TRUE(rc->getRetryMap().empty());

        // The route DEL later arrives through the normal path and the
        // removal converges
        Table appRouteTable(m_app_db.get(), APP_ROUTE_TABLE_NAME);
        appRouteTable.del("10.10.6.0/24");
        auto routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> routeEntries;
        routeEntries.push_back({"10.10.6.0/24", "DEL", {}});
        routeConsumer->addToSync(routeEntries);
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 0);

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
    }

    TEST_F(IntfsOrchTest, IntfsOrchParkedRouteCleanedByLateDel)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.8.0/24", "Ethernet0", m_app_db.get()));

        // Wedge and complete the removal: the parked intent outlives the episode
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_EQ(rc->getRetryMap().size(), 1u);

        // A route DEL arriving after parking evicts the parked intent
        Table appRouteTable(m_app_db.get(), APP_ROUTE_TABLE_NAME);
        appRouteTable.del("10.10.8.0/24");
        auto routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> routeEntries;
        routeEntries.push_back({"10.10.8.0/24", "DEL", {}});
        routeConsumer->addToSync(routeEntries);
        ASSERT_TRUE(rc->getRetryMap().empty());
        static_cast<Orch *>(gRouteOrch)->doTask();

        // Nothing replays once the interface is recreated
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.8.0/24")));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 0);
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictHostRouteKeyFormat)
    {
        // fpmsyncd writes full mask keys without the /len suffix; the parked
        // intent must be read and replayed under exactly that key
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.9.9.9", "Ethernet0", m_app_db.get()));
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.9.9.9/32")));

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.9.9.9/32")));
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_EQ(rc->getRetryMap().size(), 1u);
        ASSERT_EQ(rc->getRetryMap().begin()->first, "10.9.9.9");

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.9.9.9/32")));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);
        ASSERT_TRUE(rc->getRetryMap().empty());
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictVrfVanishBeforeReplay)
    {
        std::deque<KeyOpFieldsValuesTuple> vrfEntries;
        vrfEntries.push_back({"Vrf-Blue", "SET", { {"NULL", "NULL"}}});
        auto vrfConsumer = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
        vrfConsumer->addToSync(vrfEntries);
        static_cast<Orch *>(gVrfOrch)->doTask();
        sai_object_id_t blue_vrf_id = gVrfOrch->getVRFid("Vrf-Blue");

        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("Vrf-Blue:10.56.0.0/24", "Ethernet0", m_app_db.get()));
        ASSERT_TRUE(gRouteOrch->isRouteExists(blue_vrf_id, IpPrefix("10.56.0.0/24")));

        // Wedge, evict and complete the removal, then delete the VRF while
        // the intent is still parked
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        vrfEntries.clear();
        vrfEntries.push_back({"Vrf-Blue", "DEL", {}});
        vrfConsumer->addToSync(vrfEntries);
        static_cast<Orch *>(gVrfOrch)->doTask();
        ASSERT_FALSE(gVrfOrch->isVRFexists("Vrf-Blue"));

        // The replayed task is retained like any route SET for a missing VRF
        // (pre-existing retention semantics): no crash, no programming
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        auto routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        static_cast<Orch *>(gRouteOrch)->doTask();
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_TRUE(rc->getRetryMap().empty());
        ASSERT_EQ(routeConsumer->m_toSync.count("Vrf-Blue:10.56.0.0/24"), 1u);
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 0);

        // A later DEL for the key replaces the retained SET so the stale
        // intent can never program even if the VRF returns
        std::deque<KeyOpFieldsValuesTuple> routeEntries;
        routeEntries.push_back({"Vrf-Blue:10.56.0.0/24", "DEL", {}});
        routeConsumer->addToSync(routeEntries);
        auto pending = routeConsumer->m_toSync.find("Vrf-Blue:10.56.0.0/24");
        ASSERT_NE(pending, routeConsumer->m_toSync.end());
        ASSERT_EQ(kfvOp(pending->second), DEL_COMMAND);
        ASSERT_EQ(routeConsumer->m_toSync.count("Vrf-Blue:10.56.0.0/24"), 1u);
    }

    TEST_F(IntfsOrchTest, IntfsOrchFreshSetMergesWithParkedRoute)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.7.0/24", "Ethernet0", m_app_db.get()));

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        static_cast<Orch *>(gIntfsOrch)->doTask();
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_EQ(rc->getRetryMap().size(), 1u);

        // A fresh SET for the parked key pulls the parked intent back into
        // m_toSync and merges with it; the route programs exactly once after
        // the interface is recreated
        auto routeConsumer = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> routeEntries;
        routeEntries.push_back({"10.10.7.0/24", "SET",
            { {"nexthop", "0.0.0.0"}, {"ifname", "Ethernet0"}, {"protocol", "kernel"} }});
        routeConsumer->addToSync(routeEntries);
        ASSERT_TRUE(rc->getRetryMap().empty());
        ASSERT_EQ(routeConsumer->m_toSync.count("10.10.7.0/24"), 1u);

        // Without the interface the merged task is retained, not dropped
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_EQ(routeConsumer->m_toSync.count("10.10.7.0/24"), 1u);

        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        auto base_create_route = create_route_entries_count;
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_EQ(base_create_route + 1, create_route_entries_count);
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.7.0/24")));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);
    }

    TEST_F(IntfsOrchTest, IntfsOrchRemovalMarkerClearedOnDroppedDel)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        gIntfsOrch->increaseRouterIntfsRefCount("Ethernet0");

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        // Simulate the state after a partial removal failure: the interface
        // entry is gone while the removal marker and the DEL task remain. The
        // dropped DEL must clear the marker instead of fencing the alias
        // forever.
        gIntfsOrch->m_syncdIntfses.erase("Ethernet0");
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        ASSERT_TRUE(gIntfsOrch->m_pendingRouteEvictions.empty());
    }

    TEST_F(IntfsOrchTest, IntfsOrchMarkerSurvivesPerIpDel)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0:10.77.0.1/24", "SET", { {"scope", "global"}, {"family", "IPv4"} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();

        // Whole-interface DEL is blocked while the address remains; the per
        // address DEL that follows must not end the removal episode
        entries.clear();
        entries.push_back({"Ethernet0", "DEL", { {} }});
        intfConsumer->addToSync(entries);
        entries.clear();
        entries.push_back({"Ethernet0:10.77.0.1/24", "DEL", {}});
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));

        // The retained whole-interface DEL completes on the next drain
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().count("Ethernet0"), 0u);
    }

    TEST_F(IntfsOrchTest, IntfsOrchEvictionDeferredDuringResync)
    {
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(programIntfRoute("10.10.9.0/24", "Ethernet0", m_app_db.get()));

        // While a route resync is reconciling, the eviction is deferred, not
        // forfeited: the route stays untouched and the episode keeps a
        // pending eviction
        gRouteOrch->m_resync = true;
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "DEL", { {} }});
        auto intfConsumer = dynamic_cast<Consumer *>(gIntfsOrch->getExecutor(APP_INTF_TABLE_NAME));
        intfConsumer->addToSync(entries);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_TRUE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.9.0/24")));
        auto rc = gRouteOrch->getRetryCache(APP_ROUTE_TABLE_NAME);
        ASSERT_TRUE(rc->getRetryMap().empty());

        // Once the resync ends the deferred eviction runs on the next attempt
        gRouteOrch->m_resync = false;
        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.9.0/24")));
        ASSERT_EQ(rc->getRetryMap().size(), 1u);

        static_cast<Orch *>(gIntfsOrch)->doTask();
        ASSERT_FALSE(gIntfsOrch->isIntfChangeInProgress("Ethernet0"));
        ASSERT_NO_FATAL_FAILURE(createIntf("Ethernet0"));
        static_cast<Orch *>(gRouteOrch)->doTask();
        ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("10.10.9.0/24")));
        ASSERT_EQ(gIntfsOrch->getSyncdIntfses().at("Ethernet0").ref_count, 1);
        ASSERT_TRUE(rc->getRetryMap().empty());
    }
}
