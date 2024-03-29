// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_MINER_H
#define BITCOIN_NODE_MINER_H

#include <net.h>
#include <node/context.h>
#include <primitives/block.h>
#include <txmempool.h>
#include <wallet/wallet.h>

#include <memory>
#include <optional>
#include <stdint.h>

#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

class ChainstateManager;
class CBlockIndex;
class CChainParams;
class CScript;

using wallet::CWallet;

namespace Consensus { struct Params; };

namespace node {
static const bool DEFAULT_GENERATE = false;
static const int DEFAULT_GENERATE_THREADS = 1;

static const bool DEFAULT_PRINTPRIORITY = false;

struct CBlockTemplate
{
    CBlock block;
    std::vector<CAmount> vTxFeesCash;
    std::vector<CAmount> vTxFeesBond;
    std::vector<int64_t> vTxSigOpsCost;
    std::vector<unsigned char> vchCoinbaseCommitment;
};

// Container for tracking updates to ancestor feerate as we include (parent)
// transactions in a block
struct CTxMemPoolModifiedEntry {
    explicit CTxMemPoolModifiedEntry(CTxMemPool::txiter entry)
    {
        iter = entry;
        nSizeWithAncestors = entry->GetSizeWithAncestors();
        nModFeesWithAncestors = entry->GetModFeesWithAncestors();
        nSigOpCostWithAncestors = entry->GetSigOpCostWithAncestors();
    }

    CAmount GetModifiedFee() const { return iter->GetModifiedFee(); }
    uint64_t GetSizeWithAncestors() const { return nSizeWithAncestors; }
    CAmount GetModFeesWithAncestors() const { return nModFeesWithAncestors; }
    size_t GetTxSize() const { return iter->GetTxSize(); }
    const CTransaction& GetTx() const { return iter->GetTx(); }

    CTxMemPool::txiter iter;
    uint64_t nSizeWithAncestors;
    CAmount nModFeesWithAncestors;
    int64_t nSigOpCostWithAncestors;
};

/** Comparator for CTxMemPool::txiter objects.
 *  It simply compares the internal memory address of the CTxMemPoolEntry object
 *  pointed to. This means it has no meaning, and is only useful for using them
 *  as key in other indexes.
 */
struct CompareCTxMemPoolIter {
    bool operator()(const CTxMemPool::txiter& a, const CTxMemPool::txiter& b) const
    {
        return &(*a) < &(*b);
    }
};

struct modifiedentry_iter {
    typedef CTxMemPool::txiter result_type;
    result_type operator() (const CTxMemPoolModifiedEntry &entry) const
    {
        return entry.iter;
    }
};

// A comparator that sorts transactions based on number of ancestors.
// This is sufficient to sort an ancestor package in an order that is valid
// to appear in a block.
struct CompareTxIterByAncestorCount {
    bool operator()(const CTxMemPool::txiter& a, const CTxMemPool::txiter& b) const
    {
        if (a->GetCountWithAncestors() != b->GetCountWithAncestors()) {
            return a->GetCountWithAncestors() < b->GetCountWithAncestors();
        }
        return CompareIteratorByHash()(a, b);
    }
};

typedef boost::multi_index_container<
    CTxMemPoolModifiedEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            modifiedentry_iter,
            CompareCTxMemPoolIter
        >,
        // sorted by modified ancestor fee rate
        boost::multi_index::ordered_non_unique<
            // Reuse same tag from CTxMemPool's similar index
            boost::multi_index::tag<ancestor_score>,
            boost::multi_index::identity<CTxMemPoolModifiedEntry>,
            CompareTxMemPoolEntryByAncestorFee
        >
    >
> indexed_modified_transaction_set;

typedef indexed_modified_transaction_set::nth_index<0>::type::iterator modtxiter;
typedef indexed_modified_transaction_set::index<ancestor_score>::type::iterator modtxscoreiter;

struct update_for_parent_inclusion
{
    explicit update_for_parent_inclusion(CTxMemPool::txiter it) : iter(it) {}

    void operator() (CTxMemPoolModifiedEntry &e)
    {
        e.nModFeesWithAncestors -= iter->GetModifiedFee();
        e.nSizeWithAncestors -= iter->GetTxSize();
        e.nSigOpCostWithAncestors -= iter->GetSigOpCost();
    }

    CTxMemPool::txiter iter;
};

// Container for sorting currently invalid conversion transactions
struct CTxMemPoolConversionEntry : CTxMemPoolModifiedEntry {
    explicit CTxMemPoolConversionEntry(CTxMemPool::txiter entry, double conversionRate_, CAmountType conversionType_) : CTxMemPoolModifiedEntry(entry)
    {
        conversionRate = conversionRate_;
        conversionType = conversionType_;
    }
    double GetConversionRate() const { return conversionRate; }
    CAmountType GetConversionType() const { return conversionType; }

    double conversionRate;
    CAmountType conversionType;
};

struct conversionentry_iter {
    typedef CTxMemPool::txiter result_type;
    result_type operator() (const CTxMemPoolConversionEntry &entry) const
    {
        return entry.iter;
    }
};

class CompareTxMemPoolConversionEntryByConversionRate
{
public:
    bool operator()(const CTxMemPoolConversionEntry& a, const CTxMemPoolConversionEntry& b) const
    {
        return a.GetConversionRate() < b.GetConversionRate();
    }
};

struct index_by_conversion_rate {};

typedef boost::multi_index_container<
    CTxMemPoolConversionEntry,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            conversionentry_iter,
            CompareCTxMemPoolIter
        >,
        // sorted by conversion rate
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<index_by_conversion_rate>,
            boost::multi_index::identity<CTxMemPoolConversionEntry>,
            CompareTxMemPoolConversionEntryByConversionRate
        >
    >
> indexed_conversion_transaction_set;

typedef indexed_conversion_transaction_set::nth_index<0>::type::iterator conversiontxiter;
typedef indexed_conversion_transaction_set::index<index_by_conversion_rate>::type::iterator conversionrateiter;

/** Generate a new block, without valid proof-of-work */
class BlockAssembler
{
private:
    // The constructed block template
    std::unique_ptr<CBlockTemplate> pblocktemplate;

    // Configuration parameters for the block size
    unsigned int nBlockMaxWeight;
    CFeeRate blockMinFeeRate;

    // Information on the current status of the block
    uint64_t nBlockWeight;
    uint64_t nBlockTx;
    uint64_t nBlockSigOpsCost;
    CAmount nFees[2] = {0};
    CTxMemPool::setEntries inBlock;
    std::vector<CTxOut> conversionOutputs;

    // Chain context for the block
    int nHeight;
    int64_t m_lock_time_cutoff;

    const CChainParams& chainparams;
    const CTxMemPool* const m_mempool;
    Chainstate& m_chainstate;

public:
    struct Options {
        Options();
        size_t nBlockMaxWeight;
        CFeeRate blockMinFeeRate;
    };

    explicit BlockAssembler(Chainstate& chainstate, const CTxMemPool* mempool);
    explicit BlockAssembler(Chainstate& chainstate, const CTxMemPool* mempool, const Options& options);

    /** Construct a new block template with coinbase to scriptPubKeyIn */
    std::unique_ptr<CBlockTemplate> CreateNewBlock(const CScript& scriptPubKeyIn);

    inline static std::optional<int64_t> m_last_block_num_txs{};
    inline static std::optional<int64_t> m_last_block_weight{};

private:
    // utility functions
    /** Clear the block's state and prepare for assembling a new block */
    void resetBlock();
    /** Add a tx to the block */
    void AddToBlock(CTxMemPool::txiter iter);

    // Methods for how to add transactions to a block.
    /** Add transactions based on feerate including unconfirmed ancestors
      * Increments nPackagesSelected / nDescendantsUpdated with corresponding
      * statistics from the package selection (for logging statistics). */
    void addPackageTxs(const CTxMemPool& mempool, int& nPackagesSelected, int& nDescendantsUpdated) EXCLUSIVE_LOCKS_REQUIRED(mempool.cs);

    // helper functions for addPackageTxs()
    /** Remove confirmed (inBlock) entries from given set */
    void onlyUnconfirmed(CTxMemPool::setEntries& testSet);
    /** Test if a new package would "fit" in the block */
    bool TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const;
    /** Perform checks on each transaction in a package:
      * locktime, premature-witness, serialized size (if necessary)
      * These checks should always succeed, and they're here
      * only as an extra check in case of suboptimal node configuration */
    bool TestPackageTransactions(const CTxMemPool::setEntries& package, std::optional<CTxConversionInfo>& conversionInfo) const;
    /** Create a conversion entry with the estimated conversion rate necessary
      * to execute the transaction.
      */
    CTxMemPoolConversionEntry GetConversionEntry(const CTxMemPool::txiter& iter, const CTxConversionInfo& conversionInfo) const;
    /** Sort the package in an order that is valid to appear in a block */
    void SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries);
};

/** Modify the extranonce in a block */
void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce);
int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev);

/** Run the miner threads */
void StartMining(NodeContext& context, int nThreads, CWallet* pwallet);
void StopMining();

/** Update an old GenerateCoinbaseCommitment from CreateNewBlock after the block txs have changed */
void RegenerateCommitments(CBlock& block, ChainstateManager& chainman);
} // namespace node

#endif // BITCOIN_NODE_MINER_H
