// Copyright (c) 2018-2019 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_AVALANCHE_PROCESSOR_H
#define BITCOIN_AVALANCHE_PROCESSOR_H

#include <avalanche/node.h>
#include <avalanche/proofcomparator.h>
#include <avalanche/protocol.h>
#include <blockindexworkcomparator.h>
#include <eventloop.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <key.h>
#include <net.h>
#include <rwcollection.h>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

class ArgsManager;
class CBlockIndex;
class CConnman;
class CNode;
class CScheduler;
class Config;
class PeerManager;
struct bilingual_str;

/**
 * Maximum item that can be polled at once.
 */
static constexpr size_t AVALANCHE_MAX_ELEMENT_POLL = 16;

/**
 * How long before we consider that a query timed out.
 */
static constexpr std::chrono::milliseconds AVALANCHE_DEFAULT_QUERY_TIMEOUT{
    10000};

namespace avalanche {

class Delegation;
class PeerManager;
class Proof;
struct VoteRecord;

enum struct VoteStatus : uint8_t {
    Invalid,
    Rejected,
    Accepted,
    Finalized,
    Stale,
};

template <typename VoteItem> class VoteItemUpdate {
    VoteItem item;
    VoteStatus status;

public:
    VoteItemUpdate(const VoteItem itemIn, VoteStatus statusIn)
        : item(std::move(itemIn)), status(statusIn) {}

    const VoteStatus &getStatus() const { return status; }

    VoteItem getVoteItem() { return item; }
    const VoteItem getVoteItem() const { return item; }
};

using BlockUpdate = VoteItemUpdate<CBlockIndex *>;
using ProofUpdate = VoteItemUpdate<ProofRef>;

using BlockVoteMap =
    std::map<const CBlockIndex *, VoteRecord, CBlockIndexWorkComparator>;
using ProofVoteMap =
    std::map<const ProofRef, VoteRecord, ProofComparatorByScore>;

struct query_timeout {};

namespace {
    struct AvalancheTest;
}

// FIXME Implement a proper notification handler for node disconnection instead
// of implementing the whole NetEventsInterface for a single interesting event.
class Processor final : public NetEventsInterface {
    CConnman *connman;
    std::chrono::milliseconds queryTimeoutDuration;

    /**
     * Blocks to run avalanche on.
     */
    RWCollection<BlockVoteMap> blockVoteRecords;

    /**
     * Proofs to run avalanche on.
     */
    RWCollection<ProofVoteMap> proofVoteRecords;

    /**
     * Keep track of peers and queries sent.
     */
    std::atomic<uint64_t> round;

    /**
     * Keep track of the peers and associated infos.
     */
    mutable Mutex cs_peerManager;
    std::unique_ptr<PeerManager> peerManager GUARDED_BY(cs_peerManager);

    struct Query {
        NodeId nodeid;
        uint64_t round;
        TimePoint timeout;

        /**
         * We declare this as mutable so it can be modified in the multi_index.
         * This is ok because we do not use this field to index in anyway.
         *
         * /!\ Do not use any mutable field as index.
         */
        mutable std::vector<CInv> invs;
    };

    using QuerySet = boost::multi_index_container<
        Query,
        boost::multi_index::indexed_by<
            // index by nodeid/round
            boost::multi_index::hashed_unique<boost::multi_index::composite_key<
                Query,
                boost::multi_index::member<Query, NodeId, &Query::nodeid>,
                boost::multi_index::member<Query, uint64_t, &Query::round>>>,
            // sorted by timeout
            boost::multi_index::ordered_non_unique<
                boost::multi_index::tag<query_timeout>,
                boost::multi_index::member<Query, TimePoint,
                                           &Query::timeout>>>>;

    RWCollection<QuerySet> queries;

    /** Data required to participate. */
    struct PeerData;
    std::unique_ptr<PeerData> peerData;
    CKey sessionKey;

    /** Event loop machinery. */
    EventLoop eventLoop;

    /**
     * Quorum management.
     */
    uint32_t minQuorumScore;
    double minQuorumConnectedScoreRatio;
    std::atomic<bool> quorumIsEstablished{false};
    int64_t minAvaproofsNodeCount;
    std::atomic<int64_t> avaproofsNodeCounter{0};

    /** Voting parameters. */
    const uint32_t staleVoteThreshold;
    const uint32_t staleVoteFactor;

    /** Registered interfaces::Chain::Notifications handler. */
    class NotificationsHandler;
    std::unique_ptr<interfaces::Handler> chainNotificationsHandler;

    Processor(const ArgsManager &argsman, interfaces::Chain &chain,
              CConnman *connmanIn, CScheduler &scheduler,
              std::unique_ptr<PeerData> peerDataIn, CKey sessionKeyIn,
              uint32_t minQuorumTotalScoreIn,
              double minQuorumConnectedScoreRatioIn,
              int64_t minAvaproofsNodeCountIn, uint32_t staleVoteThresholdIn,
              uint32_t staleVoteFactorIn);

public:
    ~Processor();

    static std::unique_ptr<Processor> MakeProcessor(const ArgsManager &argsman,
                                                    interfaces::Chain &chain,
                                                    CConnman *connman,
                                                    CScheduler &scheduler,
                                                    bilingual_str &error);

    void setQueryTimeoutDuration(std::chrono::milliseconds d) {
        queryTimeoutDuration = d;
    }

    bool addBlockToReconcile(const CBlockIndex *pindex);
    bool addProofToReconcile(const ProofRef &proof);
    bool isAccepted(const CBlockIndex *pindex) const;
    bool isAccepted(const ProofRef &proof) const;
    int getConfidence(const CBlockIndex *pindex) const;
    int getConfidence(const ProofRef &proof) const;

    // TODO: Refactor the API to remove the dependency on avalanche/protocol.h
    void sendResponse(CNode *pfrom, Response response) const;
    bool registerVotes(NodeId nodeid, const Response &response,
                       std::vector<BlockUpdate> &blockUpdates,
                       std::vector<ProofUpdate> &proofUpdates, int &banscore,
                       std::string &error);

    template <typename Callable> auto withPeerManager(Callable &&func) const {
        LOCK(cs_peerManager);
        return func(*peerManager);
    }

    CPubKey getSessionPubKey() const;
    bool sendHello(CNode *pfrom) const;

    ProofRef getLocalProof() const;

    /*
     * Return whether the avalanche service flag should be set.
     */
    bool isAvalancheServiceAvailable() { return !!peerData; }

    bool startEventLoop(CScheduler &scheduler);
    bool stopEventLoop();

    void avaproofsSent(NodeId nodeid);
    int64_t getAvaproofsNodeCounter() const {
        return avaproofsNodeCounter.load();
    }
    bool isQuorumEstablished();

    // Implement NetEventInterface. Only FinalizeNode is of interest.
    void InitializeNode(const Config &config, CNode *pnode) override {}
    bool ProcessMessages(const Config &config, CNode *pnode,
                         std::atomic<bool> &interrupt) override {
        return false;
    }
    bool SendMessages(const Config &config, CNode *pnode) override {
        return false;
    }

    /** Handle removal of a node */
    void FinalizeNode(const Config &config, const CNode &node,
                      bool &update_connection_time) override;

private:
    void runEventLoop();
    void clearTimedoutRequests();
    std::vector<CInv> getInvsForNextPoll(bool forPoll = true);

    bool isWorthPolling(const CBlockIndex *pindex) const
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool isWorthPolling(const ProofRef &proof) const
        EXCLUSIVE_LOCKS_REQUIRED(cs_peerManager);

    friend struct ::avalanche::AvalancheTest;
};

} // namespace avalanche

#endif // BITCOIN_AVALANCHE_PROCESSOR_H
