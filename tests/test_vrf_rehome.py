import ipaddress
import json
import time

import pytest
from swsscommon import swsscommon

from dvslib.dvs_common import PollingConfig, wait_for_result


RIF_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
VRF_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"
ROUTE_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
POLL = PollingConfig(polling_interval=0.1, timeout=30)


def wait_for_rif(asic, port_id, vrf_id):
    def check():
        entries = {
            key: asic.get_entry(RIF_TABLE, key)
            for key in asic.get_keys(RIF_TABLE)
        }
        matches = [
            key for key, fields in entries.items()
            if fields.get("SAI_ROUTER_INTERFACE_ATTR_PORT_ID") == port_id
            and fields.get("SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID") == vrf_id
        ]
        return len(matches) == 1, matches

    return wait_for_result(check, POLL)[1][0]


def wait_for_route(asic, vrf_id, prefix, rif_id):
    def check():
        for key in asic.get_keys(ROUTE_TABLE):
            route = json.loads(key)
            if route["vr"] == vrf_id and route["dest"] == prefix:
                fields = asic.get_entry(ROUTE_TABLE, key)
                return fields.get("SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID") == rif_id, fields
        return False, {}

    wait_for_result(check, POLL)


@pytest.mark.parametrize("scenario", ["ordinary", "persistent", "delete"])
@pytest.mark.parametrize("address,peer,leaked_prefix,zero", [
    ("192.0.2.1/24", "192.0.2.2/24", "198.51.100.0/24", "0.0.0.0"),
    ("2001:db8:1::1/64", "2001:db8:1::2/64", "2001:db8:2::/64", "::"),
])
def test_config_vrf_rehome(dvs, scenario, address, peer, leaked_prefix, zero):
    """CONFIG rehome succeeds or retains the old RIF; DEL releases its obligations."""
    config = dvs.get_config_db()
    app = dvs.get_app_db()
    asic = dvs.get_asic_db()
    state = dvs.get_state_db()
    origin, target = ("VrfOrigin", "") if ":" in address else ("", "VrfTarget")
    if scenario == "persistent":
        origin, target = "VrfOrigin", "VrfTarget"
    port = "Ethernet0"
    port_id = dvs.asicdb.portnamemap[port]
    initial_vrfs = set(asic.get_keys(VRF_TABLE))
    assert len(initial_vrfs) == 1
    vrfs = {"": next(iter(initial_vrfs))}
    old_admin = config.get_entry("PORT", port).get("admin_status", "down")
    producer = swsscommon.ProducerStateTable(app.db_connection, "ROUTE_TABLE")
    route_key = "VrfRoute:" + leaked_prefix
    family = "-6" if ":" in address else "-4"
    local = str(ipaddress.ip_interface(address).ip)
    connected_prefix = str(ipaddress.ip_interface(address).network)
    created_vrfs = []
    replacement = str(ipaddress.ip_interface(address).ip + 2) + "/" + address.split("/")[1]

    def status_is(field, value):
        row = state.get_entry("INTERFACE_REHOME_TABLE", port)
        return row.get(field) == value, row

    try:
        for name in ("VrfOrigin", "VrfTarget", "VrfRoute"):
            before = set(asic.get_keys(VRF_TABLE))
            config.create_entry("VRF", name, {"empty": "empty"})
            created_vrfs.append(name)

            def created():
                added = set(asic.get_keys(VRF_TABLE)) - before
                return len(added) == 1, added

            vrfs[name] = next(iter(wait_for_result(created, POLL)[1]))

        config.update_entry("PORT", port, {"admin_status": "up"})
        config.create_entry("INTERFACE", port, {"vrf_name": origin})
        config.create_entry("INTERFACE", port + "|" + address, {"NULL": "NULL"})
        old_rif = wait_for_rif(asic, port_id, vrfs[origin])
        wait_for_route(asic, vrfs[origin], connected_prefix, old_rif)
        if scenario != "ordinary":
            producer.set(route_key, swsscommon.FieldValuePairs([
                ("ifname", port), ("nexthop", zero), ("protocol", "static")
            ]))
            wait_for_route(asic, vrfs["VrfRoute"], leaked_prefix, old_rif)
        assert dvs.servers[0].runcmd("ip {} address replace {} dev eth0".format(family, peer)) == 0

        config.update_entry("INTERFACE", port, {"vrf_name": target})

        if scenario != "ordinary":
            wait_for_result(lambda: status_is("manager_phase", "drain"), POLL)
            drain = time.monotonic()
            if scenario == "persistent":
                config.delete_entry("INTERFACE", port + "|" + address)
                config.create_entry("INTERFACE", port + "|" + replacement, {"NULL": "NULL"})
                local = str(ipaddress.ip_interface(replacement).ip)
            else:
                config.delete_entry("INTERFACE", port + "|" + address)
                config.delete_entry("INTERFACE", port)
                producer._del(route_key)
                wait_for_result(lambda: (old_rif not in asic.get_keys(RIF_TABLE), asic.get_keys(RIF_TABLE)), POLL)
                wait_for_result(lambda: (not state.get_entry("INTERFACE_REHOME_TABLE", port),
                                         state.get_entry("INTERFACE_REHOME_TABLE", port)), POLL)
                config.create_entry("INTERFACE", port, {"vrf_name": origin})
                config.create_entry("INTERFACE", port + "|" + address, {"NULL": "NULL"})
        if scenario != "delete":
            outcome = "cancelled" if scenario == "persistent" else "succeeded"
            row = wait_for_result(lambda: status_is("outcome", outcome), POLL)[1]
            assert row["manager_applied_vrf"] == (origin if scenario == "persistent" else target)
            assert config.get_entry("INTERFACE", port)["vrf_name"] == target
        applied = target if scenario == "ordinary" else origin
        new_rif = wait_for_rif(asic, port_id, vrfs[applied])
        if scenario == "persistent":
            assert 4.5 <= time.monotonic() - drain < 15
            assert new_rif == old_rif
            wait_for_route(asic, vrfs["VrfRoute"], leaked_prefix, old_rif)
            assert not config.get_entry("INTERFACE", port + "|" + address)
            assert not app.get_entry("INTF_TABLE", port + ":" + address)
        else:
            assert new_rif != old_rif
            wait_for_result(lambda: (old_rif not in asic.get_keys(RIF_TABLE), asic.get_keys(RIF_TABLE)), POLL)
        wait_for_route(asic, vrfs[applied], connected_prefix, new_rif)

        rc, output = dvs.runcmd("ip -j link show dev " + port)
        assert rc == 0
        assert json.loads(output)[0].get("master", "") == applied
        assert "UP" in json.loads(output)[0]["flags"]
        rc, output = dvs.runcmd("ip -j {} address show dev {}".format(family, port))
        assert rc == 0
        assert local in [entry["local"] for entry in json.loads(output)[0]["addr_info"]]
        if scenario == "persistent":
            assert str(ipaddress.ip_interface(address).ip) not in [entry["local"] for entry in json.loads(output)[0]["addr_info"]]

        def ping():
            rc = dvs.servers[0].runcmd("ping {} -c 1 -W 1 {}".format(family, local))
            return rc == 0, rc

        wait_for_result(ping, POLL)
        producer._del(route_key)
        wait_for_result(lambda: (not any(json.loads(key)["vr"] == vrfs["VrfRoute"] and
                                         json.loads(key)["dest"] == leaked_prefix for key in asic.get_keys(ROUTE_TABLE)),
                                 asic.get_keys(ROUTE_TABLE)), POLL)
        producer.set(route_key, swsscommon.FieldValuePairs([("ifname", port), ("nexthop", zero)]))
        wait_for_route(asic, vrfs["VrfRoute"], leaked_prefix, new_rif)
        intf_producer = swsscommon.ProducerStateTable(app.db_connection, "INTF_TABLE")
        intf_producer.set(port, swsscommon.FieldValuePairs([("loopback_action", "drop")]))

        def partial_update_applied():
            fields = asic.get_entry(RIF_TABLE, new_rif)
            return fields.get("SAI_ROUTER_INTERFACE_ATTR_LOOPBACK_PACKET_ACTION") == "SAI_PACKET_ACTION_DROP", fields

        wait_for_result(partial_update_applied, POLL)
        assert wait_for_rif(asic, port_id, vrfs[applied]) == new_rif
    finally:
        producer._del(route_key)
        config.delete_entry("INTERFACE", port + "|" + address)
        config.delete_entry("INTERFACE", port + "|" + replacement)
        config.delete_entry("INTERFACE", port)
        dvs.servers[0].runcmd("ip {} address del {} dev eth0".format(family, peer))
        config.update_entry("PORT", port, {"admin_status": old_admin})
        for name in reversed(created_vrfs):
            config.delete_entry("VRF", name)
        wait_for_result(lambda: (set(asic.get_keys(VRF_TABLE)) == initial_vrfs,
                                 asic.get_keys(VRF_TABLE)), POLL)
        wait_for_result(lambda: (not state.get_entry("INTERFACE_REHOME_TABLE", port),
                                 state.get_entry("INTERFACE_REHOME_TABLE", port)), POLL)
