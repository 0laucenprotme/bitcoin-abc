// Copyright (c) 2018-2020 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <avalanche/processor.h>

#include <avalanche/delegationbuilder.h>
#include <avalanche/peermanager.h>
#include <avalanche/proofbuilder.h>
#include <avalanche/voterecord.h>
#include <chain.h>
#include <config.h>
#include <key_io.h>
#include <net_processing.h> // For ::PeerManager
#include <reverse_iterator.h>
#include <scheduler.h>
#include <util/time.h>
#include <util/translation.h> // For bilingual_str
// D6970 moved LookupBlockIndex from chain.h to validation.h TODO: remove this
// when LookupBlockIndex is refactored out of validation
#include <validation.h>

#include <avalanche/test/util.h>
#include <test/util/setup_common.h>

#include <boost/mpl/list.hpp>
#include <boost/test/unit_test.hpp>

#include <functional>
#include <type_traits>
#include <vector>

using namespace avalanche;

namespace avalanche {
namespace {
    struct AvalancheTest {
        static void runEventLoop(avalanche::Processor &p) { p.runEventLoop(); }

        static std::vector<CInv> getInvsForNextPoll(Processor &p) {
            return p.getInvsForNextPoll(false);
        }

        static NodeId getSuitableNodeToQuery(Processor &p) {
            return WITH_LOCK(p.cs_peerManager,
                             return p.peerManager->selectNode());
        }

        static uint64_t getRound(const Processor &p) { return p.round; }

        static uint32_t getMinQuorumScore(const Processor &p) {
            return p.minQuorumScore;
        }

        static double getMinQuorumConnectedScoreRatio(const Processor &p) {
            return p.minQuorumConnectedScoreRatio;
        }

        static void clearavaproofsNodeCounter(Processor &p) {
            p.avaproofsNodeCounter = 0;
        }
    };
} // namespace
} // namespace avalanche

namespace {
struct CConnmanTest : public CConnman {
    using CConnman::CConnman;
    void AddNode(CNode &node) {
        LOCK(cs_vNodes);
        vNodes.push_back(&node);
    }
    void ClearNodes() {
        LOCK(cs_vNodes);
        for (CNode *node : vNodes) {
            delete node;
        }
        vNodes.clear();
    }
};

CService ip(uint32_t i) {
    struct in_addr s;
    s.s_addr = i;
    return CService(CNetAddr(s), Params().GetDefaultPort());
}

struct AvalancheTestingSetup : public TestChain100Setup {
    const Config &config;
    CConnmanTest *m_connman;

    std::unique_ptr<Processor> m_processor;

    // The master private key we delegate to.
    CKey masterpriv;

    AvalancheTestingSetup()
        : TestChain100Setup(), config(GetConfig()),
          masterpriv(CKey::MakeCompressedKey()) {
        // Deterministic randomness for tests.
        auto connman = std::make_unique<CConnmanTest>(config, 0x1337, 0x1337);
        m_connman = connman.get();
        m_node.connman = std::move(connman);
        m_node.peerman = ::PeerManager::make(
            config.GetChainParams(), *m_connman, m_node.banman.get(),
            *m_node.scheduler, *m_node.chainman, *m_node.mempool, false);
        m_node.chain = interfaces::MakeChain(m_node, config.GetChainParams());

        // Get the processor ready.
        bilingual_str error;
        m_processor = Processor::MakeProcessor(
            *m_node.args, *m_node.chain, m_node.connman.get(),
            *Assert(m_node.chainman), *m_node.scheduler, error);
        BOOST_CHECK(m_processor);

        gArgs.ForceSetArg("-avaproofstakeutxoconfirmations", "1");
        gArgs.ForceSetArg("-enableavalancheproofreplacement", "1");
    }

    ~AvalancheTestingSetup() {
        m_connman->ClearNodes();
        SyncWithValidationInterfaceQueue();

        gArgs.ClearForcedArg("-avaproofstakeutxoconfirmations");
        gArgs.ClearForcedArg("-enableavalancheproofreplacement");
    }

    CNode *ConnectNode(ServiceFlags nServices) {
        static NodeId id = 0;

        CAddress addr(ip(GetRandInt(0xffffffff)), NODE_NONE);
        auto node =
            new CNode(id++, ServiceFlags(NODE_NETWORK), INVALID_SOCKET, addr,
                      /* nKeyedNetGroupIn */ 0,
                      /* nLocalHostNonceIn */ 0,
                      /* nLocalExtraEntropyIn */ 0, CAddress(),
                      /* pszDest */ "", ConnectionType::OUTBOUND_FULL_RELAY,
                      /* inbound_onion */ false);
        node->SetCommonVersion(PROTOCOL_VERSION);
        node->nServices = nServices;
        m_node.peerman->InitializeNode(config, node);
        node->nVersion = 1;
        node->fSuccessfullyConnected = true;
        node->m_avalanche_state = std::make_unique<CNode::AvalancheState>();

        m_connman->AddNode(*node);
        return node;
    }

    size_t next_coinbase = 0;
    ProofRef GetProof() {
        size_t current_coinbase = next_coinbase++;
        const CTransaction &coinbase = *m_coinbase_txns[current_coinbase];
        ProofBuilder pb(0, 0, masterpriv);
        BOOST_CHECK(pb.addUTXO(COutPoint(coinbase.GetId(), 0),
                               coinbase.vout[0].nValue, current_coinbase + 1,
                               true, coinbaseKey));
        return pb.build();
    }

    bool addNode(NodeId nodeid, const ProofId &proofid) {
        return m_processor->withPeerManager([&](avalanche::PeerManager &pm) {
            return pm.addNode(nodeid, proofid);
        });
    }

    bool addNode(NodeId nodeid) {
        auto proof = GetProof();
        return m_processor->withPeerManager([&](avalanche::PeerManager &pm) {
            return pm.registerProof(proof) &&
                   pm.addNode(nodeid, proof->getId());
        });
    }

    std::array<CNode *, 8> ConnectNodes() {
        auto proof = GetProof();
        BOOST_CHECK(
            m_processor->withPeerManager([&](avalanche::PeerManager &pm) {
                return pm.registerProof(proof);
            }));
        const ProofId &proofid = proof->getId();

        std::array<CNode *, 8> nodes;
        for (CNode *&n : nodes) {
            n = ConnectNode(NODE_AVALANCHE);
            BOOST_CHECK(addNode(n->GetId(), proofid));
        }

        return nodes;
    }

    void runEventLoop() { AvalancheTest::runEventLoop(*m_processor); }

    NodeId getSuitableNodeToQuery() {
        return AvalancheTest::getSuitableNodeToQuery(*m_processor);
    }

    std::vector<CInv> getInvsForNextPoll() {
        return AvalancheTest::getInvsForNextPoll(*m_processor);
    }

    uint64_t getRound() const { return AvalancheTest::getRound(*m_processor); }

    bool registerVotes(NodeId nodeid, const avalanche::Response &response,
                       std::vector<avalanche::BlockUpdate> &blockUpdates) {
        int banscore;
        std::string error;
        std::vector<avalanche::ProofUpdate> proofUpdates;
        return m_processor->registerVotes(nodeid, response, blockUpdates,
                                          proofUpdates, banscore, error);
    }
};

struct BlockProvider {
    AvalancheTestingSetup *fixture;

    std::vector<BlockUpdate> updates;
    uint32_t invType;

    BlockProvider(AvalancheTestingSetup *_fixture)
        : fixture(_fixture), invType(MSG_BLOCK) {}

    CBlockIndex *buildVoteItem() const {
        CBlock block = fixture->CreateAndProcessBlock({}, CScript());
        const BlockHash blockHash = block.GetHash();

        LOCK(cs_main);
        return Assert(fixture->m_node.chainman)
            ->m_blockman.LookupBlockIndex(blockHash);
    }

    uint256 getVoteItemId(const CBlockIndex *pindex) const {
        return pindex->GetBlockHash();
    }

    bool registerVotes(NodeId nodeid, const avalanche::Response &response,
                       std::string &error) {
        int banscore;
        std::vector<avalanche::ProofUpdate> proofUpdates;
        return fixture->m_processor->registerVotes(
            nodeid, response, updates, proofUpdates, banscore, error);
    }
    bool registerVotes(NodeId nodeid, const avalanche::Response &response) {
        std::string error;
        return registerVotes(nodeid, response, error);
    }

    bool addToReconcile(const CBlockIndex *pindex) {
        return fixture->m_processor->addBlockToReconcile(pindex);
    }

    std::vector<Vote> buildVotesForItems(uint32_t error,
                                         std::vector<CBlockIndex *> &&items) {
        size_t numItems = items.size();

        std::vector<Vote> votes;
        votes.reserve(numItems);

        // Votes are sorted by most work first
        std::sort(items.begin(), items.end(), CBlockIndexWorkComparator());
        for (auto &item : reverse_iterate(items)) {
            votes.emplace_back(error, item->GetBlockHash());
        }

        return votes;
    }

    void invalidateItem(CBlockIndex *pindex) {
        pindex->nStatus = pindex->nStatus.withFailed();
    }
};

struct ProofProvider {
    AvalancheTestingSetup *fixture;

    std::vector<ProofUpdate> updates;
    uint32_t invType;

    ProofProvider(AvalancheTestingSetup *_fixture)
        : fixture(_fixture), invType(MSG_AVA_PROOF) {}

    ProofRef buildVoteItem() const {
        const ProofRef proof = fixture->GetProof();
        fixture->m_processor->withPeerManager([&](avalanche::PeerManager &pm) {
            BOOST_CHECK(pm.registerProof(proof));
        });
        return proof;
    }

    uint256 getVoteItemId(const ProofRef &proof) const {
        return proof->getId();
    }

    bool registerVotes(NodeId nodeid, const avalanche::Response &response,
                       std::string &error) {
        int banscore;
        std::vector<avalanche::BlockUpdate> blockUpdates;
        return fixture->m_processor->registerVotes(
            nodeid, response, blockUpdates, updates, banscore, error);
    }
    bool registerVotes(NodeId nodeid, const avalanche::Response &response) {
        std::string error;
        return registerVotes(nodeid, response, error);
    }

    bool addToReconcile(const ProofRef &proof) {
        return fixture->m_processor->addProofToReconcile(proof);
    }

    std::vector<Vote> buildVotesForItems(uint32_t error,
                                         std::vector<ProofRef> &&items) {
        size_t numItems = items.size();

        std::vector<Vote> votes;
        votes.reserve(numItems);

        // Votes are sorted by high score first
        std::sort(items.begin(), items.end(), ProofComparatorByScore());
        for (auto &item : items) {
            votes.emplace_back(error, item->getId());
        }

        return votes;
    }

    void invalidateItem(const ProofRef &proof) {
        fixture->m_processor->withPeerManager([&](avalanche::PeerManager &pm) {
            pm.rejectProof(proof->getId(),
                           avalanche::PeerManager::RejectionMode::INVALIDATE);
        });
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(processor_tests, AvalancheTestingSetup)

// FIXME A std::tuple can be used instead of boost::mpl::list after boost 1.67
using VoteItemProviders = boost::mpl::list<BlockProvider, ProofProvider>;

BOOST_AUTO_TEST_CASE(block_update) {
    CBlockIndex index;
    CBlockIndex *pindex = &index;

    std::set<VoteStatus> status{
        VoteStatus::Invalid,   VoteStatus::Rejected, VoteStatus::Accepted,
        VoteStatus::Finalized, VoteStatus::Stale,
    };

    for (auto s : status) {
        BlockUpdate abu(pindex, s);
        // The use of BOOST_CHECK instead of BOOST_CHECK_EQUAL prevents from
        // having to define operator<<() for each argument type.
        BOOST_CHECK(abu.getVoteItem() == pindex);
        BOOST_CHECK(abu.getStatus() == s);
    }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(item_reconcile_twice, P, VoteItemProviders) {
    P provider(this);

    auto item = provider.buildVoteItem();

    // Adding the item twice does nothing.
    BOOST_CHECK(provider.addToReconcile(item));
    BOOST_CHECK(!provider.addToReconcile(item));
    BOOST_CHECK(m_processor->isAccepted(item));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(item_null, P, VoteItemProviders) {
    P provider(this);

    // Check that null case is handled on the public interface
    BOOST_CHECK(!m_processor->isAccepted(nullptr));
    BOOST_CHECK_EQUAL(m_processor->getConfidence(nullptr), -1);

    auto item = decltype(provider.buildVoteItem())();
    BOOST_CHECK(item == nullptr);
    BOOST_CHECK(!provider.addToReconcile(item));

    // Check that adding item to vote on doesn't change the outcome. A
    // comparator is used under the hood, and this is skipped if there are no
    // vote records.
    item = provider.buildVoteItem();
    BOOST_CHECK(provider.addToReconcile(item));

    BOOST_CHECK(!m_processor->isAccepted(nullptr));
    BOOST_CHECK_EQUAL(m_processor->getConfidence(nullptr), -1);
}

namespace {
Response next(Response &r) {
    auto copy = r;
    r = {r.getRound() + 1, r.getCooldown(), r.GetVotes()};
    return copy;
}
} // namespace

BOOST_AUTO_TEST_CASE_TEMPLATE(vote_item_register, P, VoteItemProviders) {
    P provider(this);
    auto &updates = provider.updates;
    const uint32_t invType = provider.invType;

    const auto item = provider.buildVoteItem();
    const auto itemid = provider.getVoteItemId(item);

    // Create nodes that supports avalanche.
    auto avanodes = ConnectNodes();

    // Querying for random item returns false.
    BOOST_CHECK(!m_processor->isAccepted(item));

    // Add a new item. Check it is added to the polls.
    BOOST_CHECK(provider.addToReconcile(item));
    auto invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, invType);
    BOOST_CHECK(invs[0].hash == itemid);

    BOOST_CHECK(m_processor->isAccepted(item));

    int nextNodeIndex = 0;
    auto registerNewVote = [&](const Response &resp) {
        runEventLoop();
        auto nodeid = avanodes[nextNodeIndex++ % avanodes.size()]->GetId();
        BOOST_CHECK(provider.registerVotes(nodeid, resp));
    };

    // Let's vote for this item a few times.
    Response resp{0, 0, {Vote(0, itemid)}};
    for (int i = 0; i < 6; i++) {
        registerNewVote(next(resp));
        BOOST_CHECK(m_processor->isAccepted(item));
        BOOST_CHECK_EQUAL(m_processor->getConfidence(item), 0);
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // A single neutral vote do not change anything.
    resp = {getRound(), 0, {Vote(-1, itemid)}};
    registerNewVote(next(resp));
    BOOST_CHECK(m_processor->isAccepted(item));
    BOOST_CHECK_EQUAL(m_processor->getConfidence(item), 0);
    BOOST_CHECK_EQUAL(updates.size(), 0);

    resp = {getRound(), 0, {Vote(0, itemid)}};
    for (int i = 1; i < 7; i++) {
        registerNewVote(next(resp));
        BOOST_CHECK(m_processor->isAccepted(item));
        BOOST_CHECK_EQUAL(m_processor->getConfidence(item), i);
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // Two neutral votes will stall progress.
    resp = {getRound(), 0, {Vote(-1, itemid)}};
    registerNewVote(next(resp));
    BOOST_CHECK(m_processor->isAccepted(item));
    BOOST_CHECK_EQUAL(m_processor->getConfidence(item), 6);
    BOOST_CHECK_EQUAL(updates.size(), 0);
    registerNewVote(next(resp));
    BOOST_CHECK(m_processor->isAccepted(item));
    BOOST_CHECK_EQUAL(m_processor->getConfidence(item), 6);
    BOOST_CHECK_EQUAL(updates.size(), 0);

    resp = {getRound(), 0, {Vote(0, itemid)}};
    for (int i = 2; i < 8; i++) {
        registerNewVote(next(resp));
        BOOST_CHECK(m_processor->isAccepted(item));
        BOOST_CHECK_EQUAL(m_processor->getConfidence(item), 6);
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // We vote for it numerous times to finalize it.
    for (int i = 7; i < AVALANCHE_FINALIZATION_SCORE; i++) {
        registerNewVote(next(resp));
        BOOST_CHECK(m_processor->isAccepted(item));
        BOOST_CHECK_EQUAL(m_processor->getConfidence(item), i);
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // As long as it is not finalized, we poll.
    invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, invType);
    BOOST_CHECK(invs[0].hash == itemid);

    // Now finalize the decision.
    registerNewVote(next(resp));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getVoteItem() == item);
    BOOST_CHECK(updates[0].getStatus() == VoteStatus::Finalized);
    updates.clear();

    // Once the decision is finalized, there is no poll for it.
    invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 0);

    // Now let's undo this and finalize rejection.
    BOOST_CHECK(provider.addToReconcile(item));
    invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, invType);
    BOOST_CHECK(invs[0].hash == itemid);

    resp = {getRound(), 0, {Vote(1, itemid)}};
    for (int i = 0; i < 6; i++) {
        registerNewVote(next(resp));
        BOOST_CHECK(m_processor->isAccepted(item));
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // Now the state will flip.
    registerNewVote(next(resp));
    BOOST_CHECK(!m_processor->isAccepted(item));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getVoteItem() == item);
    BOOST_CHECK(updates[0].getStatus() == VoteStatus::Rejected);
    updates.clear();

    // Now it is rejected, but we can vote for it numerous times.
    for (int i = 1; i < AVALANCHE_FINALIZATION_SCORE; i++) {
        registerNewVote(next(resp));
        BOOST_CHECK(!m_processor->isAccepted(item));
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // As long as it is not finalized, we poll.
    invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, invType);
    BOOST_CHECK(invs[0].hash == itemid);

    // Now finalize the decision.
    registerNewVote(next(resp));
    BOOST_CHECK(!m_processor->isAccepted(item));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getVoteItem() == item);
    BOOST_CHECK(updates[0].getStatus() == VoteStatus::Invalid);
    updates.clear();

    // Once the decision is finalized, there is no poll for it.
    invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 0);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(multi_item_register, P, VoteItemProviders) {
    P provider(this);
    auto &updates = provider.updates;
    const uint32_t invType = provider.invType;

    auto itemA = provider.buildVoteItem();
    auto itemidA = provider.getVoteItemId(itemA);

    auto itemB = provider.buildVoteItem();
    auto itemidB = provider.getVoteItemId(itemB);

    // Create several nodes that support avalanche.
    auto avanodes = ConnectNodes();

    // Querying for random item returns false.
    BOOST_CHECK(!m_processor->isAccepted(itemA));
    BOOST_CHECK(!m_processor->isAccepted(itemB));

    // Start voting on item A.
    BOOST_CHECK(provider.addToReconcile(itemA));
    auto invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, invType);
    BOOST_CHECK(invs[0].hash == itemidA);

    uint64_t round = getRound();
    runEventLoop();
    BOOST_CHECK(provider.registerVotes(avanodes[0]->GetId(),
                                       {round, 0, {Vote(0, itemidA)}}));
    BOOST_CHECK_EQUAL(updates.size(), 0);

    // Start voting on item B after one vote.
    std::vector<Vote> votes = provider.buildVotesForItems(0, {itemA, itemB});
    Response resp{round + 1, 0, votes};
    BOOST_CHECK(provider.addToReconcile(itemB));
    invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 2);

    // Ensure the inv ordering is as expected
    for (size_t i = 0; i < invs.size(); i++) {
        BOOST_CHECK_EQUAL(invs[i].type, invType);
        BOOST_CHECK(invs[i].hash == votes[i].GetHash());
    }

    // Let's vote for these items a few times.
    for (int i = 0; i < 4; i++) {
        NodeId nodeid = getSuitableNodeToQuery();
        runEventLoop();
        BOOST_CHECK(provider.registerVotes(nodeid, next(resp)));
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // Now it is accepted, but we can vote for it numerous times.
    for (int i = 0; i < AVALANCHE_FINALIZATION_SCORE; i++) {
        NodeId nodeid = getSuitableNodeToQuery();
        runEventLoop();
        BOOST_CHECK(provider.registerVotes(nodeid, next(resp)));
        BOOST_CHECK_EQUAL(updates.size(), 0);
    }

    // Running two iterration of the event loop so that vote gets triggered on A
    // and B.
    NodeId firstNodeid = getSuitableNodeToQuery();
    runEventLoop();
    NodeId secondNodeid = getSuitableNodeToQuery();
    runEventLoop();

    BOOST_CHECK(firstNodeid != secondNodeid);

    // Next vote will finalize item A.
    BOOST_CHECK(provider.registerVotes(firstNodeid, next(resp)));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getVoteItem() == itemA);
    BOOST_CHECK(updates[0].getStatus() == VoteStatus::Finalized);
    updates = {};

    // We do not vote on A anymore.
    invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, invType);
    BOOST_CHECK(invs[0].hash == itemidB);

    // Next vote will finalize item B.
    BOOST_CHECK(provider.registerVotes(secondNodeid, resp));
    BOOST_CHECK_EQUAL(updates.size(), 1);
    BOOST_CHECK(updates[0].getVoteItem() == itemB);
    BOOST_CHECK(updates[0].getStatus() == VoteStatus::Finalized);
    updates = {};

    // There is nothing left to vote on.
    invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 0);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(poll_and_response, P, VoteItemProviders) {
    P provider(this);
    auto &updates = provider.updates;
    const uint32_t invType = provider.invType;

    const auto item = provider.buildVoteItem();
    const auto itemid = provider.getVoteItemId(item);

    // There is no node to query.
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), NO_NODE);

    // Create a node that supports avalanche and one that doesn't.
    ConnectNode(NODE_NONE);
    auto avanode = ConnectNode(NODE_AVALANCHE);
    NodeId avanodeid = avanode->GetId();
    BOOST_CHECK(addNode(avanodeid));

    // It returns the avalanche peer.
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), avanodeid);

    // Register an item and check it is added to the list of elements to poll.
    BOOST_CHECK(provider.addToReconcile(item));
    auto invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, invType);
    BOOST_CHECK(invs[0].hash == itemid);

    // Trigger a poll on avanode.
    uint64_t round = getRound();
    runEventLoop();

    // There is no more suitable peer available, so return nothing.
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), NO_NODE);

    // Respond to the request.
    Response resp = {round, 0, {Vote(0, itemid)}};
    BOOST_CHECK(provider.registerVotes(avanodeid, resp));
    BOOST_CHECK_EQUAL(updates.size(), 0);

    // Now that avanode fullfilled his request, it is added back to the list of
    // queriable nodes.
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), avanodeid);

    auto checkRegisterVotesError = [&](NodeId nodeid,
                                       const avalanche::Response &response,
                                       const std::string &expectedError) {
        std::string error;
        BOOST_CHECK(!provider.registerVotes(nodeid, response, error));
        BOOST_CHECK_EQUAL(error, expectedError);
        BOOST_CHECK_EQUAL(updates.size(), 0);
    };

    // Sending a response when not polled fails.
    checkRegisterVotesError(avanodeid, next(resp), "unexpected-ava-response");

    // Trigger a poll on avanode.
    round = getRound();
    runEventLoop();
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), NO_NODE);

    // Sending responses that do not match the request also fails.
    // 1. Too many results.
    resp = {round, 0, {Vote(0, itemid), Vote(0, itemid)}};
    runEventLoop();
    checkRegisterVotesError(avanodeid, resp, "invalid-ava-response-size");
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), avanodeid);

    // 2. Not enough results.
    resp = {getRound(), 0, {}};
    runEventLoop();
    checkRegisterVotesError(avanodeid, resp, "invalid-ava-response-size");
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), avanodeid);

    // 3. Do not match the poll.
    resp = {getRound(), 0, {Vote()}};
    runEventLoop();
    checkRegisterVotesError(avanodeid, resp, "invalid-ava-response-content");
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), avanodeid);

    // 4. Invalid round count. Request is not discarded.
    uint64_t queryRound = getRound();
    runEventLoop();

    resp = {queryRound + 1, 0, {Vote()}};
    checkRegisterVotesError(avanodeid, resp, "unexpected-ava-response");

    resp = {queryRound - 1, 0, {Vote()}};
    checkRegisterVotesError(avanodeid, resp, "unexpected-ava-response");

    // 5. Making request for invalid nodes do not work. Request is not
    // discarded.
    resp = {queryRound, 0, {Vote(0, itemid)}};
    checkRegisterVotesError(avanodeid + 1234, resp, "unexpected-ava-response");

    // Proper response gets processed and avanode is available again.
    resp = {queryRound, 0, {Vote(0, itemid)}};
    BOOST_CHECK(provider.registerVotes(avanodeid, resp));
    BOOST_CHECK_EQUAL(updates.size(), 0);
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), avanodeid);

    // Out of order response are rejected.
    const auto item2 = provider.buildVoteItem();
    BOOST_CHECK(provider.addToReconcile(item2));

    std::vector<Vote> votes = provider.buildVotesForItems(0, {item, item2});
    resp = {getRound(), 0, {votes[1], votes[0]}};
    runEventLoop();
    checkRegisterVotesError(avanodeid, resp, "invalid-ava-response-content");
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), avanodeid);

    // But they are accepted in order.
    resp = {getRound(), 0, votes};
    runEventLoop();
    BOOST_CHECK(provider.registerVotes(avanodeid, resp));
    BOOST_CHECK_EQUAL(updates.size(), 0);
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), avanodeid);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(dont_poll_invalid_item, P, VoteItemProviders) {
    P provider(this);
    auto &updates = provider.updates;
    const uint32_t invType = provider.invType;

    auto itemA = provider.buildVoteItem();
    auto itemB = provider.buildVoteItem();

    auto avanodes = ConnectNodes();

    // Build votes to get proper ordering
    std::vector<Vote> votes = provider.buildVotesForItems(0, {itemA, itemB});

    // Register the items and check they are added to the list of elements to
    // poll.
    BOOST_CHECK(provider.addToReconcile(itemA));
    BOOST_CHECK(provider.addToReconcile(itemB));
    auto invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 2);
    for (size_t i = 0; i < invs.size(); i++) {
        BOOST_CHECK_EQUAL(invs[i].type, invType);
        BOOST_CHECK(invs[i].hash == votes[i].GetHash());
    }

    // When an item is marked invalid, stop polling.
    provider.invalidateItem(itemB);

    Response goodResp{getRound(), 0, {Vote(0, provider.getVoteItemId(itemA))}};
    runEventLoop();
    BOOST_CHECK(provider.registerVotes(avanodes[0]->GetId(), goodResp));
    BOOST_CHECK_EQUAL(updates.size(), 0);

    // Votes including itemB are rejected
    Response badResp{getRound(), 0, votes};
    runEventLoop();
    std::string error;
    BOOST_CHECK(!provider.registerVotes(avanodes[1]->GetId(), badResp, error));
    BOOST_CHECK_EQUAL(error, "invalid-ava-response-size");
}

BOOST_TEST_DECORATOR(*boost::unit_test::timeout(60))
BOOST_AUTO_TEST_CASE_TEMPLATE(poll_inflight_timeout, P, VoteItemProviders) {
    P provider(this);

    const auto item = provider.buildVoteItem();
    const auto itemid = provider.getVoteItemId(item);

    // Add the item
    BOOST_CHECK(provider.addToReconcile(item));

    // Create a node that supports avalanche.
    auto avanode = ConnectNode(NODE_AVALANCHE);
    NodeId avanodeid = avanode->GetId();
    BOOST_CHECK(addNode(avanodeid));

    // Expire requests after some time.
    auto queryTimeDuration = std::chrono::milliseconds(10);
    m_processor->setQueryTimeoutDuration(queryTimeDuration);
    for (int i = 0; i < 10; i++) {
        Response resp = {getRound(), 0, {Vote(0, itemid)}};

        auto start = std::chrono::steady_clock::now();
        runEventLoop();
        // We cannot guarantee that we'll wait for just 1ms, so we have to bail
        // if we aren't within the proper time range.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        runEventLoop();

        bool ret = provider.registerVotes(avanodeid, next(resp));
        if (std::chrono::steady_clock::now() > start + queryTimeDuration) {
            // We waited for too long, bail. Because we can't know for sure when
            // previous steps ran, ret is not deterministic and we do not check
            // it.
            i--;
            continue;
        }

        // We are within time bounds, so the vote should have worked.
        BOOST_CHECK(ret);

        // Now try again but wait for expiration.
        runEventLoop();
        std::this_thread::sleep_for(queryTimeDuration);
        runEventLoop();
        BOOST_CHECK(!provider.registerVotes(avanodeid, next(resp)));
    }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(poll_inflight_count, P, VoteItemProviders) {
    P provider(this);
    const uint32_t invType = provider.invType;

    // Create enough nodes so that we run into the inflight request limit.
    auto proof = GetProof();
    BOOST_CHECK(m_processor->withPeerManager(
        [&](avalanche::PeerManager &pm) { return pm.registerProof(proof); }));

    std::array<CNode *, AVALANCHE_MAX_INFLIGHT_POLL + 1> nodes;
    for (auto &n : nodes) {
        n = ConnectNode(NODE_AVALANCHE);
        BOOST_CHECK(addNode(n->GetId(), proof->getId()));
    }

    // Add an item to poll
    const auto item = provider.buildVoteItem();
    const auto itemid = provider.getVoteItemId(item);
    BOOST_CHECK(provider.addToReconcile(item));

    // Ensure there are enough requests in flight.
    std::map<NodeId, uint64_t> node_round_map;
    for (int i = 0; i < AVALANCHE_MAX_INFLIGHT_POLL; i++) {
        NodeId nodeid = getSuitableNodeToQuery();
        BOOST_CHECK(node_round_map.find(nodeid) == node_round_map.end());
        node_round_map.insert(std::pair<NodeId, uint64_t>(nodeid, getRound()));
        auto invs = getInvsForNextPoll();
        BOOST_CHECK_EQUAL(invs.size(), 1);
        BOOST_CHECK_EQUAL(invs[0].type, invType);
        BOOST_CHECK(invs[0].hash == itemid);
        runEventLoop();
    }

    // Now that we have enough in flight requests, we shouldn't poll.
    auto suitablenodeid = getSuitableNodeToQuery();
    BOOST_CHECK(suitablenodeid != NO_NODE);
    auto invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 0);
    runEventLoop();
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), suitablenodeid);

    // Send one response, now we can poll again.
    auto it = node_round_map.begin();
    Response resp = {it->second, 0, {Vote(0, itemid)}};
    BOOST_CHECK(provider.registerVotes(it->first, resp));
    node_round_map.erase(it);

    invs = getInvsForNextPoll();
    BOOST_CHECK_EQUAL(invs.size(), 1);
    BOOST_CHECK_EQUAL(invs[0].type, invType);
    BOOST_CHECK(invs[0].hash == itemid);
}

BOOST_AUTO_TEST_CASE(quorum_diversity) {
    std::vector<BlockUpdate> updates;

    CBlock block = CreateAndProcessBlock({}, CScript());
    const BlockHash blockHash = block.GetHash();
    const CBlockIndex *pindex;
    {
        LOCK(cs_main);
        pindex =
            Assert(m_node.chainman)->m_blockman.LookupBlockIndex(blockHash);
    }

    // Create nodes that supports avalanche.
    auto avanodes = ConnectNodes();

    // Querying for random block returns false.
    BOOST_CHECK(!m_processor->isAccepted(pindex));

    // Add a new block. Check it is added to the polls.
    BOOST_CHECK(m_processor->addBlockToReconcile(pindex));

    // Do one valid round of voting.
    uint64_t round = getRound();
    Response resp{round, 0, {Vote(0, blockHash)}};

    // Check that all nodes can vote.
    for (size_t i = 0; i < avanodes.size(); i++) {
        runEventLoop();
        BOOST_CHECK(registerVotes(avanodes[i]->GetId(), next(resp), updates));
    }

    // Generate a query for every single node.
    const NodeId firstNodeId = getSuitableNodeToQuery();
    std::map<NodeId, uint64_t> node_round_map;
    round = getRound();
    for (size_t i = 0; i < avanodes.size(); i++) {
        NodeId nodeid = getSuitableNodeToQuery();
        BOOST_CHECK(node_round_map.find(nodeid) == node_round_map.end());
        node_round_map[nodeid] = getRound();
        runEventLoop();
    }

    // Now only the first node can vote. All others would be duplicate in the
    // quorum.
    auto confidence = m_processor->getConfidence(pindex);
    BOOST_REQUIRE(confidence > 0);

    for (auto &[nodeid, r] : node_round_map) {
        if (nodeid == firstNodeId) {
            // Node 0 is the only one which can vote at this stage.
            round = r;
            continue;
        }

        BOOST_CHECK(
            registerVotes(nodeid, {r, 0, {Vote(0, blockHash)}}, updates));
        BOOST_CHECK_EQUAL(m_processor->getConfidence(pindex), confidence);
    }

    BOOST_CHECK(
        registerVotes(firstNodeId, {round, 0, {Vote(0, blockHash)}}, updates));
    BOOST_CHECK_EQUAL(m_processor->getConfidence(pindex), confidence + 1);
}

BOOST_AUTO_TEST_CASE(event_loop) {
    CScheduler s;

    CBlock block = CreateAndProcessBlock({}, CScript());
    const BlockHash blockHash = block.GetHash();
    const CBlockIndex *pindex;
    {
        LOCK(cs_main);
        pindex =
            Assert(m_node.chainman)->m_blockman.LookupBlockIndex(blockHash);
    }

    // Starting the event loop.
    BOOST_CHECK(m_processor->startEventLoop(s));

    // There is one task planned in the next hour (our event loop).
    std::chrono::system_clock::time_point start, stop;
    BOOST_CHECK_EQUAL(s.getQueueInfo(start, stop), 1);

    // Starting twice doesn't start it twice.
    BOOST_CHECK(!m_processor->startEventLoop(s));

    // Start the scheduler thread.
    std::thread schedulerThread(std::bind(&CScheduler::serviceQueue, &s));

    // Create a node that supports avalanche.
    auto avanode = ConnectNode(NODE_AVALANCHE);
    NodeId nodeid = avanode->GetId();
    BOOST_CHECK(addNode(nodeid));

    // There is no query in flight at the moment.
    BOOST_CHECK_EQUAL(getSuitableNodeToQuery(), nodeid);

    // Add a new block. Check it is added to the polls.
    uint64_t queryRound = getRound();
    BOOST_CHECK(m_processor->addBlockToReconcile(pindex));

    for (int i = 0; i < 60 * 1000; i++) {
        // Technically, this is a race condition, but this should do just fine
        // as we wait up to 1 minute for an event that should take 10ms.
        UninterruptibleSleep(std::chrono::milliseconds(1));
        if (getRound() != queryRound) {
            break;
        }
    }

    // Check that we effectively got a request and not timed out.
    BOOST_CHECK(getRound() > queryRound);

    // Respond and check the cooldown time is respected.
    uint64_t responseRound = getRound();
    auto queryTime =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);

    std::vector<BlockUpdate> updates;
    registerVotes(nodeid, {queryRound, 100, {Vote(0, blockHash)}}, updates);
    for (int i = 0; i < 10000; i++) {
        // We make sure that we do not get a request before queryTime.
        UninterruptibleSleep(std::chrono::milliseconds(1));
        if (getRound() != responseRound) {
            BOOST_CHECK(std::chrono::steady_clock::now() > queryTime);
            break;
        }
    }

    // But we eventually get one.
    BOOST_CHECK(getRound() > responseRound);

    // Stop event loop.
    BOOST_CHECK(m_processor->stopEventLoop());

    // We don't have any task scheduled anymore.
    BOOST_CHECK_EQUAL(s.getQueueInfo(start, stop), 0);

    // Can't stop the event loop twice.
    BOOST_CHECK(!m_processor->stopEventLoop());

    // Wait for the scheduler to stop.
    s.StopWhenDrained();
    schedulerThread.join();
}

BOOST_AUTO_TEST_CASE(destructor) {
    CScheduler s;
    std::chrono::system_clock::time_point start, stop;

    std::thread schedulerThread;
    BOOST_CHECK(m_processor->startEventLoop(s));
    BOOST_CHECK_EQUAL(s.getQueueInfo(start, stop), 1);

    // Start the service thread after the queue size check to prevent a race
    // condition where the thread may be processing the event loop task during
    // the check.
    schedulerThread = std::thread(std::bind(&CScheduler::serviceQueue, &s));

    // Destroy the processor.
    m_processor.reset();

    // Now that avalanche is destroyed, there is no more scheduled tasks.
    BOOST_CHECK_EQUAL(s.getQueueInfo(start, stop), 0);

    // Wait for the scheduler to stop.
    s.StopWhenDrained();
    schedulerThread.join();
}

BOOST_AUTO_TEST_CASE(add_proof_to_reconcile) {
    uint32_t score = MIN_VALID_PROOF_SCORE;

    auto addProofToReconcile = [&](uint32_t proofScore) {
        auto proof = buildRandomProof(proofScore);
        m_processor->withPeerManager([&](avalanche::PeerManager &pm) {
            BOOST_CHECK(pm.registerProof(proof));
        });
        BOOST_CHECK(m_processor->addProofToReconcile(proof));
        return proof;
    };

    for (size_t i = 0; i < AVALANCHE_MAX_ELEMENT_POLL; i++) {
        auto proof = addProofToReconcile(++score);

        auto invs = AvalancheTest::getInvsForNextPoll(*m_processor);
        BOOST_CHECK_EQUAL(invs.size(), i + 1);
        BOOST_CHECK(invs.front().IsMsgProof());
        BOOST_CHECK_EQUAL(invs.front().hash, proof->getId());
    }

    // From here a new proof is only polled if its score is in the top
    // AVALANCHE_MAX_ELEMENT_POLL
    ProofId lastProofId;
    for (size_t i = 0; i < 10; i++) {
        auto proof = addProofToReconcile(++score);

        auto invs = AvalancheTest::getInvsForNextPoll(*m_processor);
        BOOST_CHECK_EQUAL(invs.size(), AVALANCHE_MAX_ELEMENT_POLL);
        BOOST_CHECK(invs.front().IsMsgProof());
        BOOST_CHECK_EQUAL(invs.front().hash, proof->getId());

        lastProofId = proof->getId();
    }

    for (size_t i = 0; i < 10; i++) {
        auto proof = addProofToReconcile(--score);

        auto invs = AvalancheTest::getInvsForNextPoll(*m_processor);
        BOOST_CHECK_EQUAL(invs.size(), AVALANCHE_MAX_ELEMENT_POLL);
        BOOST_CHECK(invs.front().IsMsgProof());
        BOOST_CHECK_EQUAL(invs.front().hash, lastProofId);
    }

    {
        // The score is not high enough to get polled
        auto proof = addProofToReconcile(--score);
        auto invs = AvalancheTest::getInvsForNextPoll(*m_processor);
        for (auto &inv : invs) {
            BOOST_CHECK_NE(inv.hash, proof->getId());
        }
    }

    {
        // If proof replacement is not enabled there is no point polling for the
        // proof.
        auto proof = buildRandomProof(MIN_VALID_PROOF_SCORE);
        m_processor->withPeerManager([&](avalanche::PeerManager &pm) {
            BOOST_CHECK(pm.registerProof(proof));
        });

        gArgs.ForceSetArg("-enableavalancheproofreplacement", "0");
        BOOST_CHECK(!m_processor->addProofToReconcile(proof));

        gArgs.ForceSetArg("-enableavalancheproofreplacement", "1");
        BOOST_CHECK(m_processor->addProofToReconcile(proof));

        gArgs.ClearForcedArg("-enableavalancheproofreplacement");
    }
}

BOOST_AUTO_TEST_CASE(proof_record) {
    gArgs.ForceSetArg("-avaproofstakeutxoconfirmations", "2");
    gArgs.ForceSetArg("-avalancheconflictingproofcooldown", "0");

    BOOST_CHECK(!m_processor->isAccepted(nullptr));
    BOOST_CHECK_EQUAL(m_processor->getConfidence(nullptr), -1);

    const CKey key = CKey::MakeCompressedKey();

    const COutPoint conflictingOutpoint{TxId(GetRandHash()), 0};
    const COutPoint immatureOutpoint{TxId(GetRandHash()), 0};
    {
        CScript script = GetScriptForDestination(PKHash(key.GetPubKey()));

        LOCK(cs_main);
        CCoinsViewCache &coins =
            Assert(m_node.chainman)->ActiveChainstate().CoinsTip();
        coins.AddCoin(conflictingOutpoint,
                      Coin(CTxOut(10 * COIN, script), 10, false), false);
        coins.AddCoin(immatureOutpoint,
                      Coin(CTxOut(10 * COIN, script), 100, false), false);
    }

    auto buildProof = [&](const COutPoint &outpoint, uint64_t sequence,
                          uint32_t height = 10) {
        ProofBuilder pb(sequence, 0, key);
        BOOST_CHECK(pb.addUTXO(outpoint, 10 * COIN, height, false, key));
        return pb.build();
    };

    auto conflictingProof = buildProof(conflictingOutpoint, 1);
    auto validProof = buildProof(conflictingOutpoint, 2);
    auto orphanProof = buildProof(immatureOutpoint, 3, 100);

    BOOST_CHECK(!m_processor->isAccepted(conflictingProof));
    BOOST_CHECK(!m_processor->isAccepted(validProof));
    BOOST_CHECK(!m_processor->isAccepted(orphanProof));
    BOOST_CHECK_EQUAL(m_processor->getConfidence(conflictingProof), -1);
    BOOST_CHECK_EQUAL(m_processor->getConfidence(validProof), -1);
    BOOST_CHECK_EQUAL(m_processor->getConfidence(orphanProof), -1);

    // Reconciling proofs that don't exist will fail
    BOOST_CHECK(!m_processor->addProofToReconcile(conflictingProof));
    BOOST_CHECK(!m_processor->addProofToReconcile(validProof));
    BOOST_CHECK(!m_processor->addProofToReconcile(orphanProof));

    m_processor->withPeerManager([&](avalanche::PeerManager &pm) {
        BOOST_CHECK(pm.registerProof(conflictingProof));
        BOOST_CHECK(pm.registerProof(validProof));
        BOOST_CHECK(!pm.registerProof(orphanProof));

        BOOST_CHECK(pm.isBoundToPeer(validProof->getId()));
        BOOST_CHECK(pm.isInConflictingPool(conflictingProof->getId()));
        BOOST_CHECK(pm.isOrphan(orphanProof->getId()));
    });

    BOOST_CHECK(m_processor->addProofToReconcile(conflictingProof));
    BOOST_CHECK(!m_processor->isAccepted(conflictingProof));
    BOOST_CHECK(!m_processor->isAccepted(validProof));
    BOOST_CHECK(!m_processor->isAccepted(orphanProof));
    BOOST_CHECK_EQUAL(m_processor->getConfidence(conflictingProof), 0);
    BOOST_CHECK_EQUAL(m_processor->getConfidence(validProof), -1);
    BOOST_CHECK_EQUAL(m_processor->getConfidence(orphanProof), -1);

    BOOST_CHECK(m_processor->addProofToReconcile(validProof));
    BOOST_CHECK(!m_processor->isAccepted(conflictingProof));
    BOOST_CHECK(m_processor->isAccepted(validProof));
    BOOST_CHECK(!m_processor->isAccepted(orphanProof));
    BOOST_CHECK_EQUAL(m_processor->getConfidence(conflictingProof), 0);
    BOOST_CHECK_EQUAL(m_processor->getConfidence(validProof), 0);
    BOOST_CHECK_EQUAL(m_processor->getConfidence(orphanProof), -1);

    BOOST_CHECK(!m_processor->addProofToReconcile(orphanProof));
    BOOST_CHECK(!m_processor->isAccepted(conflictingProof));
    BOOST_CHECK(m_processor->isAccepted(validProof));
    BOOST_CHECK(!m_processor->isAccepted(orphanProof));
    BOOST_CHECK_EQUAL(m_processor->getConfidence(conflictingProof), 0);
    BOOST_CHECK_EQUAL(m_processor->getConfidence(validProof), 0);
    BOOST_CHECK_EQUAL(m_processor->getConfidence(orphanProof), -1);

    gArgs.ClearForcedArg("-avaproofstakeutxoconfirmations");
    gArgs.ClearForcedArg("-avalancheconflictingproofcooldown");
}

BOOST_AUTO_TEST_CASE(quorum_detection) {
    // Set min quorum parameters for our test
    int minStake = 4'000'000;
    gArgs.ForceSetArg("-avaminquorumstake", ToString(minStake));
    gArgs.ForceSetArg("-avaminquorumconnectedstakeratio", "0.5");

    // Create a new processor with our given quorum parameters
    const auto currency = Currency::get();
    uint32_t minScore = Proof::amountToScore(minStake * currency.baseunit);

    const CKey key = CKey::MakeCompressedKey();
    auto localProof = buildRandomProof(minScore / 4, 100, key);
    gArgs.ForceSetArg("-avamasterkey", EncodeSecret(key));
    gArgs.ForceSetArg("-avaproof", localProof->ToHex());

    bilingual_str error;
    ChainstateManager &chainman = *Assert(m_node.chainman);
    std::unique_ptr<Processor> processor = Processor::MakeProcessor(
        *m_node.args, *m_node.chain, m_node.connman.get(), chainman,
        *m_node.scheduler, error);

    BOOST_CHECK(processor != nullptr);
    BOOST_CHECK(processor->getLocalProof() != nullptr);
    BOOST_CHECK_EQUAL(processor->getLocalProof()->getId(), localProof->getId());
    BOOST_CHECK_EQUAL(AvalancheTest::getMinQuorumScore(*processor), minScore);
    BOOST_CHECK_EQUAL(
        AvalancheTest::getMinQuorumConnectedScoreRatio(*processor), 0.5);

    // The local proof has not been validated yet
    processor->withPeerManager([&](avalanche::PeerManager &pm) {
        BOOST_CHECK_EQUAL(pm.getTotalPeersScore(), 0);
        BOOST_CHECK_EQUAL(pm.getConnectedPeersScore(), 0);
    });
    BOOST_CHECK(!processor->isQuorumEstablished());

    // Register the local proof. This is normally done when the chain tip is
    // updated. The local proof should be accounted for in the min quorum
    // computation but the peer manager doesn't know about that.
    processor->withPeerManager([&](avalanche::PeerManager &pm) {
        BOOST_CHECK(pm.registerProof(processor->getLocalProof()));
        BOOST_CHECK(pm.isBoundToPeer(processor->getLocalProof()->getId()));
        BOOST_CHECK_EQUAL(pm.getTotalPeersScore(), minScore / 4);
        BOOST_CHECK_EQUAL(pm.getConnectedPeersScore(), 0);
    });
    BOOST_CHECK(!processor->isQuorumEstablished());

    // Add part of the required stake and make sure we still report no quorum
    auto proof1 = buildRandomProof(minScore / 2);
    processor->withPeerManager([&](avalanche::PeerManager &pm) {
        BOOST_CHECK(pm.registerProof(proof1));
        BOOST_CHECK_EQUAL(pm.getTotalPeersScore(), 3 * minScore / 4);
        BOOST_CHECK_EQUAL(pm.getConnectedPeersScore(), 0);
    });
    BOOST_CHECK(!processor->isQuorumEstablished());

    // Add the rest of the stake, but we are still lacking connected stake
    auto proof2 = buildRandomProof(minScore / 4);
    processor->withPeerManager([&](avalanche::PeerManager &pm) {
        BOOST_CHECK(pm.registerProof(proof2));
        BOOST_CHECK_EQUAL(pm.getTotalPeersScore(), minScore);
        BOOST_CHECK_EQUAL(pm.getConnectedPeersScore(), 0);
    });
    BOOST_CHECK(!processor->isQuorumEstablished());

    // Adding a node should cause the quorum to be detected and locked-in
    processor->withPeerManager([&](avalanche::PeerManager &pm) {
        pm.addNode(0, proof2->getId());
        BOOST_CHECK_EQUAL(pm.getTotalPeersScore(), minScore);
        // The peer manager knows that proof2 has a node attached ...
        BOOST_CHECK_EQUAL(pm.getConnectedPeersScore(), minScore / 4);
    });
    // ... but the processor also account for the local proof, so we reached 50%
    BOOST_CHECK(processor->isQuorumEstablished());

    // Go back to not having enough connected nodes, but we've already latched
    // the quorum as established
    processor->withPeerManager([&](avalanche::PeerManager &pm) {
        pm.removeNode(0);
        BOOST_CHECK_EQUAL(pm.getTotalPeersScore(), minScore);
        BOOST_CHECK_EQUAL(pm.getConnectedPeersScore(), 0);
    });
    BOOST_CHECK(processor->isQuorumEstablished());

    // Remove peers one at a time and ensure the quorum stays established
    auto spendProofUtxo = [&processor, &chainman](ProofRef proof) {
        {
            LOCK(cs_main);
            CCoinsViewCache &coins = chainman.ActiveChainstate().CoinsTip();
            coins.SpendCoin(proof->getStakes()[0].getStake().getUTXO());
        }
        processor->withPeerManager([&proof](avalanche::PeerManager &pm) {
            pm.updatedBlockTip();
            BOOST_CHECK(!pm.isBoundToPeer(proof->getId()));
        });
    };

    spendProofUtxo(proof2);
    processor->withPeerManager([&](avalanche::PeerManager &pm) {
        BOOST_CHECK_EQUAL(pm.getTotalPeersScore(), 3 * minScore / 4);
        BOOST_CHECK_EQUAL(pm.getConnectedPeersScore(), 0);
    });
    BOOST_CHECK(processor->isQuorumEstablished());

    spendProofUtxo(proof1);
    processor->withPeerManager([&](avalanche::PeerManager &pm) {
        BOOST_CHECK_EQUAL(pm.getTotalPeersScore(), minScore / 4);
        BOOST_CHECK_EQUAL(pm.getConnectedPeersScore(), 0);
    });
    BOOST_CHECK(processor->isQuorumEstablished());

    spendProofUtxo(processor->getLocalProof());
    processor->withPeerManager([&](avalanche::PeerManager &pm) {
        BOOST_CHECK_EQUAL(pm.getTotalPeersScore(), 0);
        BOOST_CHECK_EQUAL(pm.getConnectedPeersScore(), 0);
    });
    BOOST_CHECK(processor->isQuorumEstablished());

    gArgs.ClearForcedArg("-avamasterkey");
    gArgs.ClearForcedArg("-avaproof");
    gArgs.ClearForcedArg("-avaminquorumstake");
    gArgs.ClearForcedArg("-avaminquorumconnectedstakeratio");
}

BOOST_AUTO_TEST_CASE(quorum_detection_parameter_validation) {
    // Create vector of tuples of:
    // <min stake, min ratio, min avaproofs messages, success bool>
    std::vector<std::tuple<std::string, std::string, std::string, bool>> tests =
        {
            // All parameters are invalid
            {"", "", "", false},
            {"-1", "-1", "-1", false},

            // Min stake is out of range
            {"-1", "0", "0", false},
            {"-0.01", "0", "0", false},
            {"21000000000000.01", "0", "0", false},

            // Min connected ratio is out of range
            {"0", "-1", "0", false},
            {"0", "1.1", "0", false},

            // Min avaproofs messages ratio is out of range
            {"0", "0", "-1", false},

            // All parameters are valid
            {"0", "0", "0", true},
            {"0.00", "0", "0", true},
            {"0.01", "0", "0", true},
            {"1", "0.1", "0", true},
            {"10", "0.5", "0", true},
            {"10", "1", "0", true},
            {"21000000000000.00", "0", "0", true},
            {"0", "0", "1", true},
            {"0", "0", "100", true},
        };

    // For each case set the parameters and check that making the processor
    // succeeds or fails as expected
    for (auto it = tests.begin(); it != tests.end(); ++it) {
        gArgs.ForceSetArg("-avaminquorumstake", std::get<0>(*it));
        gArgs.ForceSetArg("-avaminquorumconnectedstakeratio", std::get<1>(*it));
        gArgs.ForceSetArg("-avaminavaproofsnodecount", std::get<2>(*it));

        bilingual_str error;
        std::unique_ptr<Processor> processor = Processor::MakeProcessor(
            *m_node.args, *m_node.chain, m_node.connman.get(),
            *Assert(m_node.chainman), *m_node.scheduler, error);

        if (std::get<3>(*it)) {
            BOOST_CHECK(processor != nullptr);
            BOOST_CHECK(error.empty());
            BOOST_CHECK_EQUAL(error.original, "");
        } else {
            BOOST_CHECK(processor == nullptr);
            BOOST_CHECK(!error.empty());
            BOOST_CHECK(error.original != "");
        }
    }

    gArgs.ClearForcedArg("-avaminquorumstake");
    gArgs.ClearForcedArg("-avaminquorumconnectedstakeratio");
    gArgs.ClearForcedArg("-avaminavaproofsnodecount");
}

BOOST_AUTO_TEST_CASE(min_avaproofs_messages) {
    ArgsManager argsman;
    argsman.ForceSetArg("-avaminquorumstake", "0");
    argsman.ForceSetArg("-avaminquorumconnectedstakeratio", "0");

    auto checkMinAvaproofsMessages = [&](int64_t minAvaproofsMessages) {
        argsman.ForceSetArg("-avaminavaproofsnodecount",
                            ToString(minAvaproofsMessages));

        bilingual_str error;
        auto processor = Processor::MakeProcessor(
            argsman, *m_node.chain, m_node.connman.get(),
            *Assert(m_node.chainman), *m_node.scheduler, error);

        BOOST_CHECK_EQUAL(processor->isQuorumEstablished(),
                          minAvaproofsMessages <= 0);

        auto addNode = [&](NodeId nodeid) {
            auto proof = buildRandomProof(MIN_VALID_PROOF_SCORE);
            processor->withPeerManager([&](avalanche::PeerManager &pm) {
                BOOST_CHECK(pm.registerProof(proof));
                BOOST_CHECK(pm.addNode(nodeid, proof->getId()));
            });
        };

        for (int64_t i = 0; i < minAvaproofsMessages - 1; i++) {
            addNode(i);

            processor->avaproofsSent(i);
            BOOST_CHECK_EQUAL(processor->getAvaproofsNodeCounter(), i + 1);

            // Receiving again on the same node does not increase the counter
            processor->avaproofsSent(i);
            BOOST_CHECK_EQUAL(processor->getAvaproofsNodeCounter(), i + 1);

            BOOST_CHECK(!processor->isQuorumEstablished());
        }

        addNode(minAvaproofsMessages);
        processor->avaproofsSent(minAvaproofsMessages);
        BOOST_CHECK(processor->isQuorumEstablished());

        // Check the latch
        AvalancheTest::clearavaproofsNodeCounter(*processor);
        BOOST_CHECK(processor->isQuorumEstablished());
    };

    checkMinAvaproofsMessages(0);
    checkMinAvaproofsMessages(1);
    checkMinAvaproofsMessages(10);
    checkMinAvaproofsMessages(100);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(voting_parameters, P, VoteItemProviders) {
    // Check that setting voting parameters has the expected effect
    gArgs.ForceSetArg("-avastalevotethreshold",
                      ToString(AVALANCHE_VOTE_STALE_MIN_THRESHOLD));
    gArgs.ForceSetArg("-avastalevotefactor", "2");

    std::vector<std::tuple<int, int>> testCases = {
        // {number of yes votes, number of neutral votes}
        {0, AVALANCHE_VOTE_STALE_MIN_THRESHOLD},
        {AVALANCHE_FINALIZATION_SCORE + 4, AVALANCHE_FINALIZATION_SCORE - 6},
    };

    bilingual_str error;
    m_processor = Processor::MakeProcessor(
        *m_node.args, *m_node.chain, m_node.connman.get(),
        *Assert(m_node.chainman), *m_node.scheduler, error);

    BOOST_CHECK(m_processor != nullptr);
    BOOST_CHECK(error.empty());

    P provider(this);
    auto &updates = provider.updates;
    const uint32_t invType = provider.invType;

    const auto item = provider.buildVoteItem();
    const auto itemid = provider.getVoteItemId(item);

    // Create nodes that supports avalanche.
    auto avanodes = ConnectNodes();
    int nextNodeIndex = 0;

    for (auto &testCase : testCases) {
        // Add a new item. Check it is added to the polls.
        BOOST_CHECK(provider.addToReconcile(item));
        auto invs = getInvsForNextPoll();
        BOOST_CHECK_EQUAL(invs.size(), 1);
        BOOST_CHECK_EQUAL(invs[0].type, invType);
        BOOST_CHECK(invs[0].hash == itemid);

        BOOST_CHECK(m_processor->isAccepted(item));

        auto registerNewVote = [&](const Response &resp) {
            runEventLoop();
            auto nodeid = avanodes[nextNodeIndex++ % avanodes.size()]->GetId();
            BOOST_CHECK(provider.registerVotes(nodeid, resp));
        };

        // Add some confidence
        for (int i = 0; i < std::get<0>(testCase); i++) {
            Response resp = {getRound(), 0, {Vote(0, itemid)}};
            registerNewVote(next(resp));
            BOOST_CHECK(m_processor->isAccepted(item));
            BOOST_CHECK_EQUAL(m_processor->getConfidence(item),
                              i >= 6 ? i - 5 : 0);
            BOOST_CHECK_EQUAL(updates.size(), 0);
        }

        // Vote until just before item goes stale
        for (int i = 0; i < std::get<1>(testCase); i++) {
            Response resp = {getRound(), 0, {Vote(-1, itemid)}};
            registerNewVote(next(resp));
            BOOST_CHECK_EQUAL(updates.size(), 0);
        }

        // As long as it is not stale, we poll.
        invs = getInvsForNextPoll();
        BOOST_CHECK_EQUAL(invs.size(), 1);
        BOOST_CHECK_EQUAL(invs[0].type, invType);
        BOOST_CHECK(invs[0].hash == itemid);

        // Now stale
        Response resp = {getRound(), 0, {Vote(-1, itemid)}};
        registerNewVote(next(resp));
        BOOST_CHECK_EQUAL(updates.size(), 1);
        BOOST_CHECK(updates[0].getVoteItem() == item);
        BOOST_CHECK(updates[0].getStatus() == VoteStatus::Stale);
        updates.clear();

        // Once stale, there is no poll for it.
        invs = getInvsForNextPoll();
        BOOST_CHECK_EQUAL(invs.size(), 0);
    }

    gArgs.ClearForcedArg("-avastalevotethreshold");
    gArgs.ClearForcedArg("-avastalevotefactor");
}

BOOST_AUTO_TEST_SUITE_END()
