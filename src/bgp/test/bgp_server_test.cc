/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_server.h"

#include "base/logging.h"
#include "base/task.h"
#include "base/test/task_test_util.h"
#include "bgp/bgp_config.h"
#include "bgp/bgp_config_parser.h"
#include "bgp/bgp_factory.h"
#include "bgp/bgp_peer.h"
#include "bgp/bgp_session_manager.h"
#include "bgp/inet/inet_table.h"
#include "bgp/test/bgp_server_test_util.h"
#include "control-node/control_node.h"
#include "io/test/event_manager_test.h"
#include "testing/gunit.h"

using namespace boost::asio;
using namespace std;

//
// Fire state machine timers faster and reduce possible delay in this test
//
class StateMachineTest : public StateMachine {
    public:
        explicit StateMachineTest(BgpPeer *peer) : StateMachine(peer) { }
        ~StateMachineTest() { }

        void StartConnectTimer(int seconds) {
            connect_timer_->Start(10,
                boost::bind(&StateMachine::ConnectTimerExpired, this),
                boost::bind(&StateMachine::TimerErrorHanlder, this, _1, _2));
        }

        void StartOpenTimer(int seconds) {
            open_timer_->Start(10,
                boost::bind(&StateMachine::OpenTimerExpired, this),
                boost::bind(&StateMachine::TimerErrorHanlder, this, _1, _2));
        }

        void StartIdleHoldTimer() {
            if (idle_hold_time_ <= 0)
                return;

            idle_hold_timer_->Start(10,
                boost::bind(&StateMachine::IdleHoldTimerExpired, this),
                boost::bind(&StateMachine::TimerErrorHanlder, this, _1, _2));
        }
};

class BgpServerUnitTest : public ::testing::Test {
protected:
    virtual void SetUp() {
        evm_.reset(new EventManager());
        a_.reset(new BgpServerTest(evm_.get(), "A"));
        b_.reset(new BgpServerTest(evm_.get(), "B"));
        thread_.reset(new ServerThread(evm_.get()));

        a_->session_manager()->Initialize(0);
        BGP_DEBUG_UT("Created server at port: " <<
            a_->session_manager()->GetPort());
        b_->session_manager()->Initialize(0);
        BGP_DEBUG_UT("Created server at port: " <<
            b_->session_manager()->GetPort());
        thread_->Start();
    }

    virtual void TearDown() {
        task_util::WaitForIdle();
        a_->Shutdown();
        b_->Shutdown();
        task_util::WaitForIdle();
        evm_->Shutdown();
        if (thread_.get() != NULL) {
            thread_->Join();
        }
        BgpPeerTest::verbose_name(false);
    }
    void SetupPeers(int peer_count, unsigned short port_a,
                    unsigned short port_b, bool verify_keepalives,
                    uint16_t as_num1 = BgpConfigManager::kDefaultAutonomousSystem,
                    uint16_t as_num2 = BgpConfigManager::kDefaultAutonomousSystem,
                    string peer_address1 = "127.0.0.1",
                    string peer_address2 = "127.0.0.1",
                    string bgp_identifier1 = "192.168.0.10",
                    string bgp_identifier2 = "192.168.0.11",
                    string family = "inet");
    void VerifyPeers(int peer_count, bool verify_keepalives,
                     uint16_t as_num1 = BgpConfigManager::kDefaultAutonomousSystem,
                     uint16_t as_num2 = BgpConfigManager::kDefaultAutonomousSystem);
    string GetConfigStr(int peer_count,
                        unsigned short port_a, unsigned short port_b,
                        uint16_t as_num1, uint16_t as_num2,
                        string peer_address1, string peer_address2,
                        string bgp_identifier1, string bgp_identifier2,
                        string family, bool delete_config);

    auto_ptr<EventManager> evm_;
    auto_ptr<ServerThread> thread_;
    auto_ptr<BgpServerTest> a_;
    auto_ptr<BgpServerTest> b_;
};

string BgpServerUnitTest::GetConfigStr(int peer_count,
        unsigned short port_a, unsigned short port_b,
        as_t as_num1, as_t as_num2,
        string peer_address1, string peer_address2,
        string bgp_identifier1, string bgp_identifier2,
        string family, bool delete_config) {
    ostringstream config;

    config << (!delete_config ? "<config>" : "<delete>");
    config << "<bgp-router name=\'A\'>"
        "<autonomous-system>" << as_num1 << "</autonomous-system>"
        "<identifier>" << bgp_identifier1 << "</identifier>"
        "<address>" << peer_address1 << "</address>"
        "<address-families><family>" << family << "</family></address-families>"
        "<port>" << port_a << "</port>";

    for (int i = 0; i < peer_count; i++) {
        config << "<session to='B'>"
        "<address-families><family>" << family << "</family></address-families>"
        "</session>";
    }
    config << "</bgp-router>";

    config << "<bgp-router name=\'B\'>"
        "<autonomous-system>" << as_num2 << "</autonomous-system>"
        "<identifier>" << bgp_identifier2 << "</identifier>"
        "<address>" << peer_address2 << "</address>"
        "<address-families><family>" << family << "</family></address-families>"
        "<port>" << port_b << "</port>";

    for (int i = 0; i < peer_count; i++) {
        config << "<session to='A'>"
        "<address-families><family>" << family << "</family></address-families>"
        "</session>";
    }
    config << "</bgp-router>";
    config << (!delete_config ? "</config>" : "</delete>");
    
    return config.str();
}

void BgpServerUnitTest::SetupPeers(int peer_count,
        unsigned short port_a, unsigned short port_b,
        bool verify_keepalives, as_t as_num1, as_t as_num2,
        string peer_address1, string peer_address2,
        string bgp_identifier1, string bgp_identifier2,
        string family) {
    string config = GetConfigStr(peer_count, port_a, port_b, as_num1, as_num2,
                                 peer_address1, peer_address2,
                                 bgp_identifier1, bgp_identifier2,
                                 family, false);
    a_->Configure(config);
    task_util::WaitForIdle();
    b_->Configure(config);
    task_util::WaitForIdle();

    VerifyPeers(peer_count, verify_keepalives, as_num1, as_num2);
}

void BgpServerUnitTest::VerifyPeers(int peer_count,
        bool verify_keepalives, as_t as_num1, as_t as_num2) {
    BgpProto::BgpPeerType peer_type =
        (as_num1 == as_num2) ? BgpProto::IBGP : BgpProto::EBGP;
    const int peers = peer_count;
    for (int j = 0; j < peers; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);

        TASK_UTIL_EXPECT_NE(static_cast<BgpPeer *>(NULL),
                a_->FindPeerByUuid(BgpConfigManager::kMasterInstance, uuid));
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        TASK_UTIL_EXPECT_EQ(as_num1, peer_a->local_as());
        TASK_UTIL_EXPECT_EQ(peer_type, peer_a->PeerType());
        BGP_WAIT_FOR_PEER_STATE(peer_a, StateMachine::ESTABLISHED);

        TASK_UTIL_EXPECT_NE(static_cast<BgpPeer *>(NULL),
                b_->FindPeerByUuid(BgpConfigManager::kMasterInstance, uuid));
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        TASK_UTIL_EXPECT_EQ(as_num2, peer_b->local_as());
        TASK_UTIL_EXPECT_EQ(peer_type, peer_b->PeerType());
        BGP_WAIT_FOR_PEER_STATE(peer_b, StateMachine::ESTABLISHED);

        if (verify_keepalives) {

            //
            // Make sure that a few keepalives are exchanged
            //
            TASK_UTIL_EXPECT_TRUE(peer_a->get_rx_keepalive() > 2);
            TASK_UTIL_EXPECT_TRUE(peer_a->get_tr_keepalive() > 2);
            TASK_UTIL_EXPECT_TRUE(peer_b->get_rx_keepalive() > 2);
            TASK_UTIL_EXPECT_TRUE(peer_b->get_tr_keepalive() > 2);
        }
    }
}

TEST_F(BgpServerUnitTest, Connection) {

    //
    // Enable BgpPeerTest::ToString() to include uuid also in the name of the
    // peer to maintain unique-ness among multiple peering sessions between
    // two BgpServers
    //
    BgpPeerTest::verbose_name(true);
    SetupPeers(3, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), true);
}

TEST_F(BgpServerUnitTest, ChangeAsNumber1) {
    int peer_count = 3;

    BgpPeerTest::verbose_name(true);
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11");

    vector<uint32_t> flap_count_a;
    vector<uint32_t> flap_count_b;

    //
    // Note down the current flap count
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        flap_count_a.push_back(peer_a->flap_count());
        flap_count_b.push_back(peer_b->flap_count());
    }

    //
    // Modify AS Number and apply
    //
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem + 1,
               BgpConfigManager::kDefaultAutonomousSystem + 1,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11");

    //
    // Make sure that the peers did flap
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        TASK_UTIL_EXPECT_TRUE(peer_a->flap_count() > flap_count_a[j]);
        TASK_UTIL_EXPECT_TRUE(peer_b->flap_count() > flap_count_b[j]);
    }
}

TEST_F(BgpServerUnitTest, ChangeAsNumber2) {
    int peer_count = 3;

    BgpPeerTest::verbose_name(true);
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11");

    vector<uint32_t> flap_count_a;
    vector<uint32_t> flap_count_b;

    //
    // Note down the current flap count
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        flap_count_a.push_back(peer_a->flap_count());
        flap_count_b.push_back(peer_b->flap_count());
    }

    //
    // Modify AS Number and apply
    //
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem + 1,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11");

    //
    // Make sure that the peers did flap
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        TASK_UTIL_EXPECT_TRUE(peer_a->flap_count() > flap_count_a[j]);
        TASK_UTIL_EXPECT_TRUE(peer_b->flap_count() > flap_count_b[j]);

    }
}

TEST_F(BgpServerUnitTest, ChangePeerAddress) {
    int peer_count = 3;

    BgpPeerTest::verbose_name(true);
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11");

    vector<uint32_t> flap_count_a;
    vector<uint32_t> flap_count_b;

    //
    // Note down the current flap count
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        flap_count_a.push_back(peer_a->flap_count());
        flap_count_b.push_back(peer_b->flap_count());
    }

    //
    // Modify Peer Address and apply
    //
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.2",
               "192.168.1.10", "192.168.1.11");

    //
    // Make sure that the peers did flap
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        TASK_UTIL_EXPECT_TRUE(peer_a->flap_count() > flap_count_a[j]);
        TASK_UTIL_EXPECT_TRUE(peer_b->flap_count() > flap_count_b[j]);
    }
}

TEST_F(BgpServerUnitTest, ChangeBgpIdentifier) {
    int peer_count = 3;

    BgpPeerTest::verbose_name(true);
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11");

    vector<uint32_t> flap_count_a;
    vector<uint32_t> flap_count_b;

    //
    // Note down the current flap count
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        flap_count_a.push_back(peer_a->flap_count());
        flap_count_b.push_back(peer_b->flap_count());
    }

    //
    // Modify BGP Identifier and apply
    //
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.1",
               "192.168.1.10", "192.168.1.11");

    //
    // Make sure that the peers did flap
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        TASK_UTIL_EXPECT_TRUE(peer_a->flap_count() > flap_count_a[j]);
        TASK_UTIL_EXPECT_TRUE(peer_b->flap_count() > flap_count_b[j]);
    }
}

TEST_F(BgpServerUnitTest, ChangePeerAddressFamilies) {
    int peer_count = 3;

    BgpPeerTest::verbose_name(true);
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11");

    vector<uint32_t> flap_count_a;
    vector<uint32_t> flap_count_b;

    //
    // Note down the current flap count
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        flap_count_a.push_back(peer_a->flap_count());
        flap_count_b.push_back(peer_b->flap_count());
    }

    //
    // Modify peer families and apply
    //
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11", "inet-vpn");

    //
    // Make sure that the peers did flap
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        TASK_UTIL_EXPECT_TRUE(peer_a->flap_count() > flap_count_a[j]);
        TASK_UTIL_EXPECT_TRUE(peer_b->flap_count() > flap_count_b[j]);
    }
}

TEST_F(BgpServerUnitTest, AdminDown) {
    int peer_count = 3;

    BgpPeerTest::verbose_name(true);
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11");

    vector<uint32_t> flap_count_a;
    vector<uint32_t> flap_count_b;

    //
    // Note down the current flap count
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        flap_count_a.push_back(peer_a->flap_count());
        flap_count_b.push_back(peer_b->flap_count());
    }

    //
    // Set peers on A to be admin down
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        peer_a->SetAdminState(true);
    }

    //
    // Make sure that the peers did flap
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        TASK_UTIL_EXPECT_TRUE(peer_a->flap_count() > flap_count_a[j]);
        TASK_UTIL_EXPECT_TRUE(peer_b->flap_count() > flap_count_b[j]);
    }

    //
    // Set peers on A to be admin up
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        peer_a->SetAdminState(false);
    }

    //
    // Make sure that the sessions come up
    //
    VerifyPeers(peer_count, false);
}

//
// BGP Port change number change is not supported yet
//
TEST_F(BgpServerUnitTest, DISABLED_ChangeBgpPort) {
    int peer_count = 1;

    BgpPeerTest::verbose_name(true);
    SetupPeers(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11");

    vector<uint32_t> flap_count_a;
    vector<uint32_t> flap_count_b;
    vector<BgpPeer *> peers_a;
    vector<BgpPeer *> peers_b;

    //
    // Note down the current flap count
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        flap_count_a.push_back(peer_a->flap_count());
        flap_count_b.push_back(peer_b->flap_count());
        peers_a.push_back(peer_a);
        peers_b.push_back(peer_b);
    }

    //
    // Remove the peers from 'B' as 'A's port shall be changed
    //
    string config = GetConfigStr(peer_count, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(),
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11", "inet", true);
    b_->Configure(config);

    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        TASK_UTIL_EXPECT_EQ(static_cast<BgpPeer *>(NULL),
                b_->FindPeerByUuid(BgpConfigManager::kMasterInstance, uuid));
    }

    //
    // Modify BGP A's TCP Server Port number and apply.
    //
    SetupPeers(peer_count, a_->session_manager()->GetPort() + 2,
               b_->session_manager()->GetPort(), false,
               BgpConfigManager::kDefaultAutonomousSystem,
               BgpConfigManager::kDefaultAutonomousSystem,
               "127.0.0.1", "127.0.0.1",
               "192.168.0.10", "192.168.0.11");

    //
    // Make sure that peers did flap.
    //
    for (int j = 0; j < peer_count; j++) {
        string uuid = BgpConfigParser::session_uuid("A", "B", j + 1);
        BgpPeer *peer_a = a_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        BgpPeer *peer_b = b_->FindPeerByUuid(BgpConfigManager::kMasterInstance,
                                             uuid);
        TASK_UTIL_EXPECT_TRUE(peer_a->flap_count() > flap_count_a[j]);
        TASK_UTIL_EXPECT_EQ(peers_a[j], peer_a);
        TASK_UTIL_EXPECT_NE(peers_b[j], peer_b);
    }
}

TEST_F(BgpServerUnitTest, BasicAdvertiseWithdraw) {
    SetupPeers(1, a_->session_manager()->GetPort(),
               b_->session_manager()->GetPort(), false);

    // Find the inet.0 table in A and B.
    DB *db_a = a_.get()->database();
    InetTable *table_a = static_cast<InetTable *>(db_a->FindTable("inet.0"));
    assert(table_a);
    DB *db_b = b_.get()->database();
    InetTable *table_b = static_cast<InetTable *>(db_b->FindTable("inet.0"));
    assert(table_b);

    // Create a BgpAttrSpec to mimic a eBGP learnt route with Origin, AS Path
    // NextHop and Local Pref.
    BgpAttrSpec attr_spec;

    BgpAttrOrigin origin(BgpAttrOrigin::IGP);
    attr_spec.push_back(&origin);

    AsPathSpec path_spec;
    AsPathSpec::PathSegment *path_seg = new AsPathSpec::PathSegment;
    path_seg->path_segment_type = AsPathSpec::PathSegment::AS_SEQUENCE;
    path_seg->path_segment.push_back(65534);
    path_spec.path_segments.push_back(path_seg);
    attr_spec.push_back(&path_spec);

    BgpAttrNextHop nexthop(0x7f00007f);
    attr_spec.push_back(&nexthop);

    BgpAttrLocalPref local_pref(100);
    attr_spec.push_back(&local_pref);

    BgpAttrPtr attr_ptr = a_.get()->attr_db()->Locate(attr_spec);

    // Create 3 IPv4 prefixes and the corresponding keys.
    const Ip4Prefix prefix1(Ip4Prefix::FromString("192.168.1.0/24"));
    const Ip4Prefix prefix2(Ip4Prefix::FromString("192.168.2.0/24"));
    const Ip4Prefix prefix3(Ip4Prefix::FromString("192.168.3.0/24"));

    const InetTable::RequestKey key1(prefix1, NULL);
    const InetTable::RequestKey key2(prefix2, NULL);
    const InetTable::RequestKey key3(prefix3, NULL);

    DBRequest req;

    // Add prefix1 to A and make sure it shows up at B.
    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
    req.key.reset(new InetTable::RequestKey(prefix1, NULL));
    req.data.reset(new InetTable::RequestData(attr_ptr, 0, 0));
    table_a->Enqueue(&req);
    task_util::WaitForIdle();

    BGP_VERIFY_ROUTE_PRESENCE(table_a, &key1);
    BGP_VERIFY_ROUTE_PRESENCE(table_b, &key1);

    // Add prefix2 to A and make sure it shows up at B.
    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
    req.key.reset(new InetTable::RequestKey(prefix2, NULL));
    req.data.reset(new InetTable::RequestData(attr_ptr, 0, 0));
    table_a->Enqueue(&req);
    task_util::WaitForIdle();

    BGP_VERIFY_ROUTE_COUNT(table_a, 2);
    BGP_VERIFY_ROUTE_COUNT(table_b, 2);
    BGP_VERIFY_ROUTE_PRESENCE(table_a, &key2);
    BGP_VERIFY_ROUTE_PRESENCE(table_b, &key2);

    // Delete prefix1 from A and make sure it's gone from B.
    req.oper = DBRequest::DB_ENTRY_DELETE;
    req.key.reset(new InetTable::RequestKey(prefix1, NULL));
    table_a->Enqueue(&req);
    task_util::WaitForIdle();

    BGP_VERIFY_ROUTE_COUNT(table_a, 1);
    BGP_VERIFY_ROUTE_COUNT(table_b, 1);
    BGP_VERIFY_ROUTE_ABSENCE(table_a, &key1);
    BGP_VERIFY_ROUTE_ABSENCE(table_b, &key1);

    // Add prefix1 and prefix3 to A and make sure they show up at B.
    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
    req.key.reset(new InetTable::RequestKey(prefix1, NULL));
    req.data.reset(new InetTable::RequestData(attr_ptr, 0, 0));
    table_a->Enqueue(&req);
    req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
    req.key.reset(new InetTable::RequestKey(prefix3, NULL));
    req.data.reset(new InetTable::RequestData(attr_ptr, 0, 0));
    table_a->Enqueue(&req);
    task_util::WaitForIdle();

    BGP_VERIFY_ROUTE_COUNT(table_a, 3);
    BGP_VERIFY_ROUTE_COUNT(table_b, 3);
    BGP_VERIFY_ROUTE_PRESENCE(table_a, &key1);
    BGP_VERIFY_ROUTE_PRESENCE(table_a, &key2);
    BGP_VERIFY_ROUTE_PRESENCE(table_a, &key3);
    BGP_VERIFY_ROUTE_PRESENCE(table_b, &key1);
    BGP_VERIFY_ROUTE_PRESENCE(table_b, &key2);
    BGP_VERIFY_ROUTE_PRESENCE(table_b, &key3);

    // Delete all the prefixes from A and make sure they are gone from B.
    req.oper = DBRequest::DB_ENTRY_DELETE;
    req.key.reset(new InetTable::RequestKey(prefix3, NULL));
    table_a->Enqueue(&req);
    req.oper = DBRequest::DB_ENTRY_DELETE;
    req.key.reset(new InetTable::RequestKey(prefix1, NULL));
    table_a->Enqueue(&req);
    req.oper = DBRequest::DB_ENTRY_DELETE;
    req.key.reset(new InetTable::RequestKey(prefix2, NULL));
    table_a->Enqueue(&req);
    task_util::WaitForIdle();

    BGP_VERIFY_ROUTE_COUNT(table_a, 0);
    BGP_VERIFY_ROUTE_COUNT(table_b, 0);
    BGP_VERIFY_ROUTE_ABSENCE(table_a, &key1);
    BGP_VERIFY_ROUTE_ABSENCE(table_a, &key2);
    BGP_VERIFY_ROUTE_ABSENCE(table_a, &key3);
    BGP_VERIFY_ROUTE_ABSENCE(table_b, &key1);
    BGP_VERIFY_ROUTE_ABSENCE(table_b, &key2);
    BGP_VERIFY_ROUTE_ABSENCE(table_b, &key3);
}

class TestEnvironment : public ::testing::Environment {
    virtual ~TestEnvironment() { }
};

static void SetUp() {
    ControlNode::SetDefaultSchedulingPolicy();
    BgpServerTest::GlobalSetUp();
    BgpObjectFactory::Register<StateMachine>(
        boost::factory<StateMachineTest *>());
}

static void TearDown() {
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    scheduler->Terminate();
}

int main(int argc, char **argv) {
    bgp_log_test::init();
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new TestEnvironment());
    SetUp();
    int result = RUN_ALL_TESTS();
    TearDown();
    return result;
}
