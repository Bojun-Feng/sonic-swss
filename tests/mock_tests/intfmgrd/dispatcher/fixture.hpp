#pragma once
// Dependency seam only. The daemon main, sweep, Consumer execute/drain and
// IntfMgr per-consumer queue handler are the actual generated production code.
// No Redis/SAI/Linux teardown implementation is supplied or claimed here.
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <typeinfo>
#include <vector>

using namespace std;
template<class... T> void testLog(T&&...) {}
#define SWSS_LOG_ENTER() ((void)0)
#define SWSS_LOG_NOTICE(...) testLog(__VA_ARGS__)
#define SWSS_LOG_ERROR(...) testLog(__VA_ARGS__)

namespace swss {
using FieldValueTuple = pair<string, string>;
using KeyOpFieldsValuesTuple = tuple<string, string, vector<FieldValueTuple>>;
inline const string& kfvKey(const KeyOpFieldsValuesTuple& t) { return get<0>(t); }
inline const string& kfvOp(const KeyOpFieldsValuesTuple& t) { return get<1>(t); }
inline const vector<FieldValueTuple>& kfvFieldsValues(const KeyOpFieldsValuesTuple& t) { return get<2>(t); }
static const string SET_COMMAND = "SET", DEL_COMMAND = "DEL";
static const string CFG_INTF_TABLE_NAME = "INTERFACE", CFG_LAG_INTF_TABLE_NAME = "PORTCHANNEL_INTERFACE";
static const string CFG_VLAN_INTF_TABLE_NAME = "VLAN_INTERFACE", CFG_LOOPBACK_INTERFACE_TABLE_NAME = "LOOPBACK_INTERFACE";
static const string CFG_VLAN_SUB_INTF_TABLE_NAME = "VLAN_SUB_INTERFACE", CFG_VOQ_INBAND_INTERFACE_TABLE_NAME = "VOQ_INBAND_INTERFACE";
static const string CFG_SAG_TABLE_NAME = "SAG", STATE_PORT_TABLE_NAME = "PORT_TABLE", STATE_LAG_TABLE_NAME = "LAG_TABLE";
static const char config_db_key_delimiter = '|';
inline vector<string> tokenize(const string& key, char delim) {
    vector<string> parts; size_t start = 0, end;
    while ((end = key.find(delim, start)) != string::npos) {
        parts.push_back(key.substr(start, end - start)); start = end + 1;
    }
    parts.push_back(key.substr(start)); return parts;
}
struct Logger { static void linkToDbNative(const char*) {} };
struct MacAddress { MacAddress() = default; explicit MacAddress(const string&) {} };
struct DBConnector { DBConnector(const char*, int) {} };
struct WarmStart {
    static void initialize(const char*, const char*) {}
    static void checkWarmStart(const char*, const char*) {}
    static bool isWarmStart() { return false; }
};
struct Table {
    Table() = default; Table(DBConnector*, const char*) {}
    bool hget(const char*, const char*, string& value) { value = "02:00:00:00:00:01"; return true; }
    void set(const string&, const vector<FieldValueTuple>&) { throw logic_error("unexpected APP write in scheduler fixture"); }
    void hset(const string&, const string&, const string&) { throw logic_error("unexpected STATE write in scheduler fixture"); }
};
struct EndSchedule {}; // Not std::exception: stops unchanged infinite daemon loop.
struct Event {
    int result; string table; vector<KeyOpFieldsValuesTuple> rows;
    bool openBefore = false, openDuring = false;
};
struct Scenario {
    vector<Event> events;
    size_t cursor = 0, selected = 0, timeouts = 0, errors = 0, attempts = 0, stateCallbacks = 0;
    bool ready = false;
    vector<size_t> attemptsAtSelect;
    map<string, size_t> completedAt;
    vector<string> pending;
    vector<string> trace;
};
extern Scenario* active;
class Selectable { public: virtual ~Selectable() = default; };
class NotificationConsumer : public Selectable {
    string channel;
public:
    NotificationConsumer(DBConnector*, const string& name) : channel(name) {}
    const string& getChannelName() const { return channel; }
    void pop(string& op, string& data, vector<FieldValueTuple>& values) {
        op = "ack"; data = "wake-only"; values.clear();
        active->trace.push_back("notification:" + channel);
    }
};
class Executor : public Selectable {
    string name;
public:
    explicit Executor(string n) : name(std::move(n)) {}
    const string& getName() const { return name; }
    virtual void execute() = 0;
    virtual void drain() = 0;
    // IntfMgr daemon has no route-ring initialization. No ring claim is tested.
    void processAnyTask(function<void()>&& task) { task(); }
};
class Consumer;
class Orch {
public:
    map<string, shared_ptr<Executor>> m_consumerMap;
    virtual ~Orch() = default;
    virtual void doTask(); // Actual production definition.
    virtual void doTask(Consumer&) = 0;
    size_t retryToSync(const string&, size_t) { return 0; } // IntfMgr configures no RetryCache.
    vector<Selectable*> getSelectables() {
        vector<Selectable*> result;
        for (const auto& item : m_consumerMap) result.push_back(item.second.get());
        return result;
    }
};
struct Input {
    string name;
    void pops(deque<KeyOpFieldsValuesTuple>& entries) {
        ++active->selected;
        active->trace.push_back("execute:" + name);
        const auto& event = active->events.at(active->cursor - 1);
        entries.insert(entries.end(), event.rows.begin(), event.rows.end());
    }
};
class Consumer : public Executor {
    Input input;
public:
    Orch* m_orch;
    multimap<string, KeyOpFieldsValuesTuple> m_toSync;
    deque<KeyOpFieldsValuesTuple> m_toSyncQueue;
    Consumer(const string& name, Orch* owner) : Executor(name), input{name}, m_orch(owner) {}
    Input* getConsumerTable() { return &input; }
    const string& getTableName() const { return getName(); }
    void addToSync(const shared_ptr<deque<KeyOpFieldsValuesTuple>>& entries) {
        // Only fresh distinct initial keys are injected. Transport/coalescing is NOT tested.
        for (const auto& row : *entries) m_toSync.emplace(kfvKey(row), row);
    }
    void execute() override; // Actual production definitions.
    void drain() override;
};
class IntfMgr : public Orch {
    Table m_appIntfTableProducer, m_stateIntfTable;
    set<string> m_pendingReplayIntfList;
    bool m_replayDone = true;
    bool doIntfGeneralTask(const vector<string>& keys, vector<FieldValueTuple>, const string& op) {
        if (op != DEL_COMMAND) throw logic_error("scheduler fixture supports root DEL only");
        ++active->attempts;
        active->trace.push_back("retry:" + keys.at(0));
        if (!active->ready) return false;
        active->completedAt.emplace(keys.at(0), active->cursor - 1);
        return true;
    }
    bool doIntfAddrTask(const vector<string>&, const vector<FieldValueTuple>&, const string&) {
        throw logic_error("prefix behavior is not part of this scheduler fixture");
    }
    void doSagTask(const vector<string>&, const vector<FieldValueTuple>&, const string&) {
        throw logic_error("SAG behavior is not part of this scheduler fixture");
    }
    void doPortTableTask(const string&, vector<FieldValueTuple>, string) {
        ++active->stateCallbacks;
        if (active->events.at(active->cursor - 1).openDuring) active->ready = true;
    }
    void setWarmReplayDoneState() { throw logic_error("warm restart is not tested"); }
    void doTask(Consumer&) override; // Actual production queue-retention handler.
public:
    using Orch::doTask;
    IntfMgr(DBConnector*, DBConnector*, DBConnector*, const vector<string>& tables) {
        for (const auto& name : tables) m_consumerMap.emplace(name, make_shared<Consumer>(name, this));
        for (const auto& name : {STATE_PORT_TABLE_NAME, STATE_LAG_TABLE_NAME})
            m_consumerMap.emplace(name, make_shared<Consumer>(name, this));
    }
    ~IntfMgr() override {
        for (const auto& item : m_consumerMap) {
            auto* consumer = static_cast<Consumer*>(item.second.get());
            for (const auto& row : consumer->m_toSync) active->pending.push_back(row.first);
        }
    }
};
class Select {
    map<string, Selectable*> inputs;
public:
    enum { ERROR = -1, OBJECT = 0, TIMEOUT = 1 };
    void addSelectable(NotificationConsumer* value) {
        inputs.emplace(value->getChannelName(), value);
    }
    void addSelectables(const vector<Selectable*>& values) {
        for (auto* value : values) inputs.emplace(static_cast<Executor*>(value)->getName(), value);
    }
    int select(Selectable** selected, int timeout) {
        if (timeout != 1000) throw logic_error("idle timeout changed");
        active->attemptsAtSelect.push_back(active->attempts);
        if (active->cursor == active->events.size()) throw EndSchedule{};
        const auto& event = active->events[active->cursor++];
        if (event.openBefore) active->ready = true;
        *selected = nullptr;
        if (event.result == OBJECT) *selected = inputs.at(event.table);
        else if (event.result == TIMEOUT) ++active->timeouts;
        else { ++active->errors; errno = EINTR; }
        return event.result;
    }
};
} // namespace swss
static size_t gBatchSize = 128;
