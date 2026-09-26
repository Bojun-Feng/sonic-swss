"""Real CONFIG_DB rebind regression. No synthetic APP/ASIC records or guard acks."""
import json
import time

import pytest

RIFS = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
VRFS = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"


def wait_for(check, description, timeout=30):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = check()
        if last:
            return last
        time.sleep(0.05)
    raise AssertionError(f"Timed out: {description}; last={last!r}")


def command(dvs, text):
    status, output = dvs.runcmd(text)
    assert status == 0, (text, status, output)
    return output


def port_rifs(asic):
    return {key: asic.get_entry(RIFS, key) for key in asic.get_keys(RIFS)
            if asic.get_entry(RIFS, key).get("SAI_ROUTER_INTERFACE_ATTR_TYPE") ==
            "SAI_ROUTER_INTERFACE_TYPE_PORT"}


@pytest.mark.parametrize("prefix", ["192.0.2.1/24", "2001:db8:1::1/64"])
def test_coalesced_default_to_named_binding_uses_new_rif(dvs, prefix):
    """Pause only orchagent, letting real managers/netlink/Redis race normally.

    The identical test is expected to expose the old-RIF bug in upstream and
    pass with the fix. A passing Linux ping alone is not its SAI ownership oracle.
    """
    cfg, state, asic = dvs.get_config_db(), dvs.get_state_db(), dvs.get_asic_db()
    alias, vrf = "Ethernet0", "VrfGuardBlue"
    ip_key = f"{alias}|{prefix}"
    initial_vrfs = set(asic.get_keys(VRFS))
    stopped = False
    pid = None
    try:
        cfg.create_entry("VRF", vrf, {"empty": "empty"})
        new_vrfs = wait_for(lambda: set(asic.get_keys(VRFS)) - initial_vrfs, "new ASIC VRF")
        assert len(new_vrfs) == 1
        target_vrf = new_vrfs.pop()
        cfg.update_entry("PORT", alias, {"admin_status": "up"})
        cfg.create_entry("INTERFACE", alias, {"NULL": "NULL"})
        cfg.create_entry("INTERFACE", ip_key, {"NULL": "NULL"})
        wait_for(lambda: state.get_entry("INTERFACE_TABLE", ip_key), "initial address")
        initial_rifs = wait_for(lambda: port_rifs(asic), "initial port RIF")
        assert len(initial_rifs) == 1, "Run this focused race on a clean VS"
        old_id, old_rif = next(iter(initial_rifs.items()))
        assert old_rif["SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID"] != target_vrf

        pid = int(command(dvs, "supervisorctl pid orchagent").strip())
        command(dvs, f"kill -STOP {pid}")
        stopped = True
        cfg.delete_entry("INTERFACE", ip_key)
        wait_for(lambda: not state.get_entry("INTERFACE_TABLE", ip_key), "independent address removal")
        cfg.delete_entry("INTERFACE", alias)
        wait_for(lambda: not state.get_entry("INTERFACE_TABLE", alias), "independent default-root removal")

        # No future target was supplied to the completed removal above.
        cfg.create_entry("INTERFACE", alias, {"vrf_name": vrf})
        cfg.create_entry("INTERFACE", ip_key, {"NULL": "NULL"})
        # Observe progress, not a blind scheduling sleep. Upstream publishes the
        # new root; the candidate publishes prepare and waits for the held fence.
        wait_for(lambda: state.get_entry("INTERFACE_TABLE", alias) or
                 state.get_entry("INTERFACE_GUARD_TABLE", alias).get("action") == "prepare",
                 "bind publication or prepare")
        print("PAUSED", json.dumps({"root": state.get_entry("INTERFACE_TABLE", alias),
              "guard": state.get_entry("INTERFACE_GUARD_TABLE", alias),
              "rifs": port_rifs(asic)}, sort_keys=True))
        command(dvs, f"kill -CONT {pid}")
        stopped = False
        wait_for(lambda: state.get_entry("INTERFACE_TABLE", alias).get("vrf") == vrf,
                 "ordinary named binding completion")
        wait_for(lambda: state.get_entry("INTERFACE_TABLE", ip_key), "restored address")

        def correct_owner():
            entries = port_rifs(asic)
            return entries if len(entries) == 1 and all(
                row["SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID"] == target_vrf
                for row in entries.values()) else None

        try:
            new_rifs = wait_for(correct_owner, "RIF belongs to the new VRF")
        finally:
            print("FINAL", json.dumps({"old_rif": old_id, "target_vrf": target_vrf,
                  "root": state.get_entry("INTERFACE_TABLE", alias),
                  "guard": state.get_entry("INTERFACE_GUARD_TABLE", alias),
                  "rifs": port_rifs(asic)}, sort_keys=True))
        assert old_id not in new_rifs
        kernel = json.loads(command(dvs, f"ip -j link show dev {alias}"))
        assert kernel[0].get("master") == vrf
    finally:
        if stopped:
            command(dvs, f"kill -CONT {pid}")
        cfg.delete_entry("INTERFACE", ip_key)
        wait_for(lambda: not state.get_entry("INTERFACE_TABLE", ip_key), "cleanup address")
        cfg.delete_entry("INTERFACE", alias)
        wait_for(lambda: not state.get_entry("INTERFACE_TABLE", alias), "cleanup root")
        cfg.delete_entry("VRF", vrf)
        wait_for(lambda: set(asic.get_keys(VRFS)) == initial_vrfs, "cleanup VRF")
