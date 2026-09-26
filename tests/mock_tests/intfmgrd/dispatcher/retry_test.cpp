#include "fixture.hpp"
#include <iostream>

using namespace swss;
Scenario* swss::active = nullptr;
#include "orch_sweep.inc"
#include "consumer_execute.inc"
#include "consumer_drain.inc"
#include "intfmgr_handler.inc"
#define main intfmgrd_main
#include "intfmgrd_under_test.cpp"
#undef main

static void require(bool condition, const string& message) {
    if (!condition) throw runtime_error(message);
}
static Event root(const string& table = CFG_INTF_TABLE_NAME, const string& alias = "Ethernet0") {
    return {Select::OBJECT, table, {{alias, DEL_COMMAND, {}}}, false, false};
}
static Event state(const string& table, bool openBefore = false, bool openDuring = false) {
    return {Select::OBJECT, table, {{"unrelated", SET_COMMAND, {}}}, openBefore, openDuring};
}
static Scenario run(vector<Event> events) {
    Scenario scenario;
    scenario.events = std::move(events);
    active = &scenario;
    bool exhausted = false;
    try { intfmgrd_main(0, nullptr); }
    catch (const EndSchedule&) { exhausted = true; }
    active = nullptr;
    require(exhausted, "daemon exited before schedule completed");
    require(scenario.cursor == scenario.events.size(), "did not service every event");
    return scenario;
}
static void completed(const Scenario& result, const string& alias, size_t step) {
    require(result.completedAt.count(alias) == 1, alias + " stayed pending after prerequisite became ready");
    require(result.completedAt.at(alias) == step, alias + " was not retried on first eligible iteration");
    require(result.pending.empty(), "pending queue not emptied");
}
static void busy(const string& table) {
    vector<Event> events{root()};
    for (size_t i = 0; i < 32; ++i) events.push_back(state(table, i == 7));
    auto result = run(events);
    require(result.timeouts == 0, "busy schedule unexpectedly had TIMEOUT");
    require(result.stateCallbacks == 32, "unrelated consumer was starved");
    completed(result, "Ethernet0", 8);
}

int main() {
    vector<pair<string, function<void()>>> tests{
#ifdef TEST_GUARD_ACK
        {"guard_notification_wakes_retry_without_TIMEOUT", [] {
            auto result = run({root(), {Select::OBJECT, "INTF_GUARD_ACK", {}, true, false}});
            completed(result, "Ethernet0", 1);
            require(result.timeouts == 0, "ack waited for idle timeout");
            require(find(result.trace.begin(), result.trace.end(), "notification:INTF_GUARD_ACK") != result.trace.end(),
                    "notification was not consumed before the retry sweep");
        }},
#endif
        {"busy_PORT_retries_owning_CONFIG_without_TIMEOUT", [] { busy(STATE_PORT_TABLE_NAME); }},
        {"busy_LAG_retries_owning_CONFIG_without_TIMEOUT", [] { busy(STATE_LAG_TABLE_NAME); }},
        {"mixed_events_retry_all_pending_interface_consumers", [] {
            auto result = run({root(), root(CFG_VLAN_INTF_TABLE_NAME, "Vlan100"),
                               root(CFG_LOOPBACK_INTERFACE_TABLE_NAME, "Loopback0"),
                               state(STATE_LAG_TABLE_NAME, true), state(STATE_PORT_TABLE_NAME)});
            for (const auto& alias : {"Ethernet0", "Vlan100", "Loopback0"}) completed(result, alias, 3);
            require(result.timeouts == 0, "unexpected timeout");
        }},
        {"selected_callback_precedes_retry_sweep", [] {
            auto result = run({root(), state(STATE_PORT_TABLE_NAME, false, true)});
            completed(result, "Ethernet0", 1);
            const auto event = find(result.trace.begin(), result.trace.end(), "execute:PORT_TABLE");
            require(event != result.trace.end(), "state executor not run");
            require(find(event, result.trace.end(), "retry:Ethernet0") != result.trace.end(),
                    "no retry after selected executor");
        }},
        {"idle_TIMEOUT_still_retries", [] {
            auto result = run({root(), {Select::TIMEOUT, "", {}, true, false}});
            completed(result, "Ethernet0", 1);
            require(result.timeouts == 1 && result.selected == 1, "timeout dispatched a selectable");
        }},
        {"same_CONFIG_event_still_retries", [] {
            auto result = run({root(), {Select::OBJECT, CFG_INTF_TABLE_NAME, {}, true, false}});
            completed(result, "Ethernet0", 1);
            require(result.timeouts == 0, "unexpected timeout");
        }},
        {"blocked_prerequisite_retains_DEL_without_spin", [] {
            vector<Event> events{root()};
            for (size_t i = 0; i < 16; ++i)
                events.push_back(state(i % 2 ? STATE_PORT_TABLE_NAME : STATE_LAG_TABLE_NAME));
            auto result = run(events);
            require(result.pending == vector<string>{"Ethernet0"}, "blocked DEL was lost");
            require(result.completedAt.empty(), "false completion while prerequisite closed");
            require(result.stateCallbacks == 16, "unrelated event lost");
            require(result.attempts <= events.size() + 1, "retry spun within a select iteration");
        }},
        {"select_ERROR_does_not_dispatch_or_sweep", [] {
            auto result = run({root(), {Select::ERROR, "", {}, true, false},
                               {Select::TIMEOUT, "", {}, false, false}});
            completed(result, "Ethernet0", 2);
            require(result.errors == 1 && result.selected == 1, "ERROR dispatched null selectable");
            require(result.attemptsAtSelect.at(1) == result.attemptsAtSelect.at(2), "ERROR caused retry");
        }},
        {"empty_pending_work_is_harmless", [] {
            auto result = run({state(STATE_PORT_TABLE_NAME), {Select::TIMEOUT, "", {}, false, false},
                               state(STATE_LAG_TABLE_NAME)});
            require(result.attempts == 0 && result.pending.empty(), "invented pending root work");
            require(result.stateCallbacks == 2, "state event dropped");
        }},
    };
    size_t failed = 0;
    for (const auto& test : tests) {
        try { test.second(); cout << "PASS " << test.first << '\n'; }
        catch (const exception& error) {
            ++failed; cout << "FAIL " << test.first << ": " << error.what() << '\n';
        }
    }
    cout << "SUMMARY passed=" << tests.size() - failed << " failed=" << failed << '\n';
    return failed ? 1 : 0;
}
