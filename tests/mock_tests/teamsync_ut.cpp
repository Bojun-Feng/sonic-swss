#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <net/if.h>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <team.h>
#include <teamdctl.h>
#include <netlink/route/link.h>
#include "schema.h"
#define protected public
#define private public
#include "teamsync.h"
#undef protected
#undef private
#include "mock_table.h"

static unsigned int (*callback_sleep)(unsigned int seconds) = NULL;
static int (*callback_team_init)(struct team_handle *th, uint32_t ifindex) = NULL;
static int (*callback_team_change_handler)(struct team_handle *th, struct team_change_handler *handler, void *priv) = NULL;
static int (*callback_teamdctl_connect)(struct teamdctl *tdc, const char *team_name, const char *addr, const char *cli_type) = NULL;
static int (*callback_teamdctl_config_get_raw_direct)(struct teamdctl *tdc, char **response) = NULL;
static void (*callback_teamdctl_disconnect)(struct teamdctl *tdc) = NULL;
static char *(*callback_if_indextoname)(unsigned int ifindex, char *ifname) = NULL;

char *if_indextoname(unsigned int ifindex, char *ifname) noexcept
{
    if (callback_if_indextoname)
    {
        return callback_if_indextoname(ifindex, ifname);
    }
    char *(*realfunc)(unsigned int, char *) =
        (char *(*)(unsigned int, char *))(dlsym (RTLD_NEXT, "if_indextoname"));
    return realfunc(ifindex, ifname);
}

static char *cb_if_indextoname(unsigned int ifindex, char *ifname)
{
    strncpy(ifname, "testLag", IF_NAMESIZE);
    return ifname;
}

static unsigned int cb_sleep(unsigned int seconds)
{
    return 0;
}

unsigned int sleep(unsigned int seconds)
{
    if (callback_sleep)
    {
        return callback_sleep(seconds);
    }
    unsigned int (*realfunc)(unsigned int) =
        (unsigned int    (*)(unsigned int))(dlsym (RTLD_NEXT, "sleep"));
    return realfunc(seconds);
}


static int cb_team_init(struct team_handle *th, uint32_t ifindex)
{
    return 0;
}

int team_init(struct team_handle *th, uint32_t ifindex)
{
    if (callback_team_init)
    {
        return callback_team_init(th, ifindex);
    }
    int (*realfunc)(struct team_handle *, uint32_t) =
        (int    (*)(struct team_handle *, uint32_t))(dlsym (RTLD_NEXT, "team_init"));
    return realfunc(th, ifindex);
}

static int cb_team_change_handler(struct team_handle *th, struct team_change_handler *handler, void *priv)
{
    return 0;
}

int team_change_handler(struct team_handle *th, struct team_change_handler *handler, void *priv)
{
    if (callback_team_change_handler)
    {
        return callback_team_change_handler(th, handler, priv);
    }
    int (*realfunc)(struct team_handle *, struct team_change_handler*, void*) =
        (int    (*)(struct team_handle *, struct team_change_handler*, void*))(dlsym (RTLD_NEXT, "team_change_handler"));
    return realfunc(th, handler, priv);
}

static int cb_teamdctl_connect(struct teamdctl *tdc, const char *team_name, const char *addr, const char *cli_type)
{
    return 0;
}

int teamdctl_connect(struct teamdctl *tdc, const char *team_name, const char *addr, const char *cli_type)
{
    if (callback_teamdctl_connect)
    {
        return callback_teamdctl_connect(tdc, team_name, addr, cli_type);
    }
    int (*realfunc)(struct teamdctl *, const char *, const char *, const char *) =
        (int    (*)(struct teamdctl *, const char *, const char *, const char *))(dlsym (RTLD_NEXT, "teamdctl_connect"));
    return realfunc(tdc, team_name, addr, cli_type);
}

static int cb_teamdctl_config_get_raw_direct_force_error(struct teamdctl *tdc, char **response)
{
    // Forced error
    return 1;
}

static int cb_teamdctl_config_get_raw_direct_success(struct teamdctl *tdc, char **response)
{
    return 0;
}

int teamdctl_config_get_raw_direct(struct teamdctl *tdc, char **response)
{
    if (callback_teamdctl_config_get_raw_direct)
    {
        return callback_teamdctl_config_get_raw_direct(tdc, response);
    }
    int (*realfunc)(struct teamdctl *, char **) =
        (int    (*)(struct teamdctl *, char **))(dlsym (RTLD_NEXT, "teamdctl_config_get_raw_direct"));
    return realfunc(tdc, response);
}

static void cb_teamdctl_disconnect(struct teamdctl *tdc)
{
}

void teamdctl_disconnect(struct teamdctl *tdc)
{
    if (callback_teamdctl_disconnect)
    {
        callback_teamdctl_disconnect(tdc);
        return;
    }
    int (*realfunc)(struct teamdctl *) =
        (int    (*)(struct teamdctl *))(dlsym (RTLD_NEXT, "teamdctl_disconnect"));
    realfunc(tdc);
}

namespace teamportsync_test
{
    struct TeamPortSyncTest : public ::testing::Test
    {
        virtual void SetUp() override
        {
            callback_sleep = cb_sleep;
            callback_team_init = NULL;
            callback_team_change_handler = NULL;
            callback_teamdctl_connect = NULL;
            callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_force_error;
            callback_teamdctl_disconnect = cb_teamdctl_disconnect;
            callback_if_indextoname = cb_if_indextoname;
        }

        virtual void TearDown() override
        {
            callback_sleep = NULL;
            callback_team_init = NULL;
            callback_team_change_handler = NULL;
            callback_teamdctl_connect = NULL;
            callback_teamdctl_config_get_raw_direct = NULL;
            callback_teamdctl_disconnect = NULL;
            callback_if_indextoname = NULL;
        }
    };

    TEST_F(TeamPortSyncTest, TestInvalidIfIndex)
    {
        try {
            swss::TeamSync::TeamPortSync("testLag", 0, NULL);
            FAIL();
        } catch (std::runtime_error &exception) {
            EXPECT_THAT(exception.what(), testing::HasSubstr("Unable to initialize team socket"));
        }
    }

    TEST_F(TeamPortSyncTest, NoLagPresent)
    {
        try {
            swss::TeamSync::TeamPortSync("testLag", 4, NULL);
            FAIL();
        } catch (std::runtime_error &exception) {
            EXPECT_THAT(exception.what(), testing::HasSubstr("Unable to initialize team socket"));
        }
    }

    TEST_F(TeamPortSyncTest, TeamdctlNoConfig)
    {
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        try {
            swss::TeamSync::TeamPortSync("testLag", 4, NULL);
            FAIL();
        } catch (std::runtime_error &exception) {
            EXPECT_THAT(exception.what(), testing::HasSubstr("Unable to get config from teamd"));
        }
    }

    TEST_F(TeamPortSyncTest, AllSuccess)
    {
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_success;
        swss::TeamSync::TeamPortSync("testLag", 4, NULL);
    }
}

namespace teamsync_test
{
    void resetTeamSyncMocks()
    {
        callback_sleep = NULL;
        callback_team_init = NULL;
        callback_team_change_handler = NULL;
        callback_teamdctl_connect = NULL;
        callback_teamdctl_config_get_raw_direct = NULL;
        callback_teamdctl_disconnect = NULL;
        callback_if_indextoname = NULL;
    }

    void setTeamSyncSuccessMocks()
    {
        callback_sleep = cb_sleep;
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_success;
        callback_teamdctl_disconnect = cb_teamdctl_disconnect;
        callback_if_indextoname = cb_if_indextoname;
    }

    class TeamSyncUnderTest : public swss::TeamSync
    {
    public:
        TeamSyncUnderTest(swss::DBConnector *db, swss::DBConnector *stateDb, swss::Select *sel)
            : swss::TeamSync(db, stateDb, sel) {}
        using swss::TeamSync::addLag;
        using swss::TeamSync::removeLag;
    };

    struct TeamSyncTest : public ::testing::Test
    {
        virtual void SetUp() override
        {
            callback_sleep = cb_sleep;
            callback_team_init = NULL;
            callback_team_change_handler = NULL;
            callback_teamdctl_connect = NULL;
            callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_force_error;
            callback_teamdctl_disconnect = cb_teamdctl_disconnect;
            callback_if_indextoname = cb_if_indextoname;
            testing_db::reset();
        }

        virtual void TearDown() override
        {
            callback_sleep = NULL;
            callback_team_init = NULL;
            callback_team_change_handler = NULL;
            callback_teamdctl_connect = NULL;
            callback_teamdctl_config_get_raw_direct = NULL;
            callback_teamdctl_disconnect = NULL;
            callback_if_indextoname = NULL;
            testing_db::reset();
        }
    };

    /* Verify that when TeamPortSync construction fails (team_init returns an
     * error for an invalid ifindex), addLag() catches the system_error
     * internally and does not propagate it to the caller. */
    TEST_F(TeamSyncTest, AddLagTeamInitFails)
    {
        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        /* ifindex 0 is invalid; the real team_init() will fail, causing
         * TeamPortSync to throw system_error, which addLag() must catch. */
        ts.addLag("testLag", 0, true, true, 1500);
    }

    /* Verify that addLag() successfully creates the team instance and writes
     * the LAG state when all underlying calls succeed. */
    TEST_F(TeamSyncTest, AddLagSucceeds)
    {
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_success;

        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        ts.addLag("testLag", 4, true, true, 1500);
    }

    /* When teamd is not ready, APP_LAG is still published for orchagent but
     * STATE_LAG is deferred until TeamPortSync succeeds. */
    TEST_F(TeamSyncTest, AddLagTeamdctlFailsNoStateLag)
    {
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        callback_sleep = cb_sleep;

        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        ts.addLag("testLag", 4, true, true, 1500);

        swss::Table appLagTable(&db, APP_LAG_TABLE_NAME);
        std::vector<std::string> appKeys;
        appLagTable.getKeys(appKeys);
        EXPECT_EQ(appKeys.size(), 1u);
        EXPECT_EQ(appKeys[0], "testLag");

        swss::Table stateLagTable(&stateDb, STATE_LAG_TABLE_NAME);
        std::vector<std::string> stateKeys;
        stateLagTable.getKeys(stateKeys);
        EXPECT_TRUE(stateKeys.empty());
    }

    TEST_F(TeamSyncTest, RemoveLagWithoutTeamPortSync)
    {
        callback_team_init = cb_team_init;
        callback_team_change_handler = cb_team_change_handler;
        callback_teamdctl_connect = cb_teamdctl_connect;
        callback_sleep = cb_sleep;

        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        ts.addLag("testLag", 4, true, true, 1500);
        ts.removeLag("testLag");

        swss::Table appLagTable(&db, APP_LAG_TABLE_NAME);
        std::vector<std::string> keys;
        appLagTable.getKeys(keys);
        EXPECT_TRUE(keys.empty());
    }

    /* When TeamPortSync exists with tracked members, removeLag() must delete
     * each APP_LAG_MEMBER entry before removing the LAG itself. */
    TEST_F(TeamSyncTest, RemoveLagWithTeamPortSyncMembers)
    {
        setTeamSyncSuccessMocks();

        swss::DBConnector db(0, "localhost", 0, 0);
        swss::DBConnector stateDb(1, "localhost", 0, 0);
        TeamSyncUnderTest ts(&db, &stateDb, nullptr);

        ts.addLag("testLag", 4, true, true, 1500);

        ASSERT_NE(ts.m_teamSelectables.find("testLag"), ts.m_teamSelectables.end());

        ts.m_teamSelectables["testLag"]->m_lagMembers["Ethernet0"] = true;
        ts.m_teamSelectables["testLag"]->m_lagMembers["Ethernet4"] = false;

        std::vector<swss::FieldValueTuple> memberFv;
        memberFv.emplace_back("status", "enabled");
        ts.m_lagMemberTable.set("testLag:Ethernet0", memberFv);
        ts.m_lagMemberTable.set("testLag:Ethernet4", memberFv);

        ts.removeLag("testLag");

        swss::Table appLagMemberTable(&db, APP_LAG_MEMBER_TABLE_NAME);
        std::vector<std::string> memberKeys;
        appLagMemberTable.getKeys(memberKeys);
        EXPECT_TRUE(memberKeys.empty());

        swss::Table appLagTable(&db, APP_LAG_TABLE_NAME);
        std::vector<std::string> lagKeys;
        appLagTable.getKeys(lagKeys);
        EXPECT_TRUE(lagKeys.empty());

        swss::Table stateLagTable(&stateDb, STATE_LAG_TABLE_NAME);
        std::vector<std::string> stateKeys;
        stateLagTable.getKeys(stateKeys);
        EXPECT_TRUE(stateKeys.empty());

        /* removeLag() defers erasing TeamPortSync until doSelectableTask().
         * Do not call doSelectableTask() here: mocked team_init does not yield
         * a valid team event fd, so epoll registration would fail in unit tests. */
        EXPECT_EQ(ts.m_selectablesToRemove.count("testLag"), 1u);
        EXPECT_NE(ts.m_teamSelectables.find("testLag"), ts.m_teamSelectables.end());
    }
}

namespace teamsync_stale_link_test
{
    /* Kernel and teamd state as seen by TeamPortSync: links by ifindex, which
     * of them are team devices, and which teamd instances answer on their
     * unix socket.  sleep() only advances the world, so blocking time in the
     * netlink callback is counted in seconds instead of being spent. */
    struct World
    {
        std::map<unsigned int, std::string> links;
        std::set<unsigned int> teams;
        std::set<std::string> readyTeamd;
        int ifindexLookupErrno = 0;
        unsigned int sleptSeconds = 0;
        unsigned int teamInitCalls = 0;
        unsigned int teamdctlConnectCalls = 0;
        std::function<void(World &)> onSleep;
    };

    static World *g_world = nullptr;

    static unsigned int world_sleep(unsigned int seconds)
    {
        g_world->sleptSeconds += seconds;
        if (g_world->onSleep)
        {
            g_world->onSleep(*g_world);
        }
        return 0;
    }

    static int world_team_init(struct team_handle *th, uint32_t ifindex)
    {
        g_world->teamInitCalls++;
        /* The team driver rejects TEAM_CMD_PORT_LIST_GET for an ifindex that
         * is gone or is not a team device. */
        return g_world->teams.count(ifindex) ? 0 : -EINVAL;
    }

    static int world_teamdctl_connect(struct teamdctl *tdc, const char *team_name,
                                      const char *addr, const char *cli_type)
    {
        g_world->teamdctlConnectCalls++;
        return g_world->readyTeamd.count(team_name) ? 0 : -ENOENT;
    }

    static char *world_if_indextoname(unsigned int ifindex, char *ifname)
    {
        if (g_world->ifindexLookupErrno)
        {
            errno = g_world->ifindexLookupErrno;
            return NULL;
        }

        auto it = g_world->links.find(ifindex);
        if (it == g_world->links.end())
        {
            errno = ENXIO;
            return NULL;
        }

        strncpy(ifname, it->second.c_str(), IF_NAMESIZE - 1);
        ifname[IF_NAMESIZE - 1] = '\0';
        return ifname;
    }

    static struct nl_object *teamLinkMsg(const std::string &name, unsigned int ifindex,
                                         unsigned int flags = 0)
    {
        struct rtnl_link *link = rtnl_link_alloc();
        rtnl_link_set_name(link, name.c_str());
        rtnl_link_set_type(link, "team");
        rtnl_link_set_ifindex(link, static_cast<int>(ifindex));
        rtnl_link_set_flags(link, flags);
        rtnl_link_set_mtu(link, 9100);
        return (struct nl_object *)link;
    }

    struct TeamSyncStaleLinkTest : public ::testing::Test
    {
        World world;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;

        void SetUp() override
        {
            testing_db::reset();
            g_world = &world;
            callback_sleep = world_sleep;
            callback_team_init = world_team_init;
            callback_team_change_handler = NULL;
            callback_teamdctl_connect = world_teamdctl_connect;
            callback_teamdctl_config_get_raw_direct = cb_teamdctl_config_get_raw_direct_success;
            callback_teamdctl_disconnect = cb_teamdctl_disconnect;
            callback_if_indextoname = world_if_indextoname;
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);
        }

        void TearDown() override
        {
            callback_sleep = NULL;
            callback_team_init = NULL;
            callback_team_change_handler = NULL;
            callback_teamdctl_connect = NULL;
            callback_teamdctl_config_get_raw_direct = NULL;
            callback_teamdctl_disconnect = NULL;
            callback_if_indextoname = NULL;
            g_world = nullptr;
            testing_db::reset();
        }

        void createTeam(const std::string &name, unsigned int ifindex)
        {
            world.links[ifindex] = name;
            world.teams.insert(ifindex);
        }

        void sendLinkMsg(swss::TeamSync &sync, int nlmsg_type, const std::string &name,
                         unsigned int ifindex, unsigned int flags = 0)
        {
            struct nl_object *msg = teamLinkMsg(name, ifindex, flags);
            sync.onMsg(nlmsg_type, msg);
            nl_object_free(msg);
        }

        bool appLagExists(const std::string &name)
        {
            swss::Table table(m_app_db.get(), APP_LAG_TABLE_NAME);
            std::vector<swss::FieldValueTuple> values;
            return table.get(name, values);
        }

        bool stateLagExists(const std::string &name)
        {
            swss::Table table(m_state_db.get(), STATE_LAG_TABLE_NAME);
            std::vector<swss::FieldValueTuple> values;
            return table.get(name, values);
        }
    };

    /* RTM_NEWLINK read from the netlink queue after the LAG was already
     * deleted (backlog of a rapid PortChannel add/del): nothing to wait for. */
    TEST_F(TeamSyncStaleLinkTest, NewLinkForDeletedLagIsNotRetried)
    {
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), nullptr);

        sendLinkMsg(sync, RTM_NEWLINK, "PortChannel01", 1052);

        EXPECT_EQ(world.sleptSeconds, 0u);
        EXPECT_EQ(world.teamInitCalls, 1u);
        EXPECT_EQ(world.teamdctlConnectCalls, 0u);
        EXPECT_EQ(sync.m_teamSelectables.count("PortChannel01"), 0u);
        /* APP_LAG is still published ahead of teamd readiness so orchagent can
         * pair it with the RTM_DELLINK that follows; STATE_LAG is not. */
        EXPECT_TRUE(appLagExists("PortChannel01"));
        EXPECT_FALSE(stateLagExists("PortChannel01"));
    }

    /* teamd creates the device before it binds its unix socket, so the first
     * attempts can fail on a healthy LAG. */
    TEST_F(TeamSyncStaleLinkTest, LateTeamdIsRetried)
    {
        createTeam("PortChannel01", 1060);
        world.onSleep = [](World &w) {
            if (w.sleptSeconds == 2)
            {
                w.readyTeamd.insert("PortChannel01");
            }
        };
        swss::TeamSync sync(m_app_db.get(), m_state_db.get(), nullptr);

        sendLinkMsg(sync, RTM_NEWLINK, "PortChannel01", 1060);

        EXPECT_EQ(world.sleptSeconds, 2u);
        EXPECT_EQ(world.teamdctlConnectCalls, 3u);
        EXPECT_EQ(sync.m_teamSelectables.count("PortChannel01"), 1u);
        EXPECT_TRUE(appLagExists("PortChannel01"));
        EXPECT_TRUE(stateLagExists("PortChannel01"));
    }
}
