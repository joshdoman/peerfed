// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/miner.h>

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/conversion.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <crypto/blake3.h>
#include <deploymentstatus.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <shutdown.h>
#include <timedata.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <util/threadnames.h>
#include <validation.h>
#include <validationinterface.h>

#include <algorithm>
#include <thread>
#include <utility>
#include <vector>

namespace node {
void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)); // TODO: Add COINBASE_FLAGS
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime{std::max<int64_t>(pindexPrev->GetMedianTimePast() + 1, TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime()))};

    if (nOldTime < nNewTime) {
        pblock->nTime = nNewTime;
    }

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks) {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);
    }

    return nNewTime - nOldTime;
}

void RegenerateCommitments(CBlock& block, ChainstateManager& chainman)
{
    CMutableTransaction tx{*block.vtx.at(0)};
    tx.vout.erase(tx.vout.begin() + GetWitnessCommitmentIndex(block));
    block.vtx.at(0) = MakeTransactionRef(tx);

    const CBlockIndex* prev_block = WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock));
    chainman.GenerateCoinbaseCommitment(block, prev_block);

    block.hashMerkleRoot = BlockMerkleRoot(block);
}

BlockAssembler::Options::Options()
{
    blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    nBlockMaxWeight = DEFAULT_BLOCK_MAX_WEIGHT;
}

BlockAssembler::BlockAssembler(Chainstate& chainstate, const CTxMemPool* mempool, const Options& options)
    : chainparams{chainstate.m_chainman.GetParams()},
      m_mempool(mempool),
      m_chainstate(chainstate)
{
    blockMinFeeRate = options.blockMinFeeRate;
    // Limit weight to between 4K and MAX_BLOCK_WEIGHT-4K for sanity:
    nBlockMaxWeight = std::max<size_t>(4000, std::min<size_t>(MAX_BLOCK_WEIGHT - 4000, options.nBlockMaxWeight));
}

static BlockAssembler::Options DefaultOptions()
{
    // Block resource limits
    // If -blockmaxweight is not given, limit to DEFAULT_BLOCK_MAX_WEIGHT
    BlockAssembler::Options options;
    options.nBlockMaxWeight = gArgs.GetIntArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT);
    if (gArgs.IsArgSet("-blockmintxfee")) {
        std::optional<CAmount> parsed = ParseMoney(gArgs.GetArg("-blockmintxfee", ""));
        options.blockMinFeeRate = CFeeRate{parsed.value_or(DEFAULT_BLOCK_MIN_TX_FEE)};
    } else {
        options.blockMinFeeRate = CFeeRate{DEFAULT_BLOCK_MIN_TX_FEE};
    }
    return options;
}

BlockAssembler::BlockAssembler(Chainstate& chainstate, const CTxMemPool* mempool)
    : BlockAssembler(chainstate, mempool, DefaultOptions()) {}

void BlockAssembler::resetBlock()
{
    inBlock.clear();
    conversionOutputs.clear();

    // Reserve space for coinbase tx
    nBlockWeight = 4000;
    nBlockSigOpsCost = 400;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees[CASH] = 0;
    nFees[BOND] = 0;
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn)
{
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if (!pblocktemplate.get()) {
        return nullptr;
    }
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxFeesCash.push_back(-1); // updated at end
    pblocktemplate->vTxFeesBond.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

    LOCK(::cs_main);
    CBlockIndex* pindexPrev = m_chainstate.m_chain.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    // Set cash and bond supply equal to the supply at the end of the previous block
    pblock->cashSupply = pindexPrev->cashSupply;
    pblock->bondSupply = pindexPrev->bondSupply;

    pblock->nVersion = m_chainstate.m_chainman.m_versionbitscache.ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand()) {
        pblock->nVersion = gArgs.GetIntArg("-blockversion", pblock->nVersion);
    }

    pblock->nTime = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime());
    m_lock_time_cutoff = pindexPrev->GetMedianTimePast();

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    if (m_mempool) {
        LOCK(m_mempool->cs);
        addPackageTxs(*m_mempool, nPackagesSelected, nDescendantsUpdated);
    }

    int64_t nTime1 = GetTimeMicros();

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    // Calculate reward and update total supply
    CAmounts reward = GetBlockSubsidy(nHeight, pblock->GetTotalSupply(), chainparams.GetConsensus());
    pblock->cashSupply += reward[CASH];
    pblock->bondSupply += reward[BOND];
    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    // Add miner outputs
    coinbaseTx.vout.resize(2); // 2 outputs to miner (1 for cash, 1 for bond)
    coinbaseTx.vout[CASH].amountType = CASH;
    coinbaseTx.vout[BOND].amountType = BOND;
    coinbaseTx.vout[CASH].scriptPubKey = scriptPubKeyIn;
    coinbaseTx.vout[BOND].scriptPubKey = scriptPubKeyIn;
    coinbaseTx.vout[CASH].nValue = nFees[CASH] + reward[CASH];
    coinbaseTx.vout[BOND].nValue = nFees[BOND] + reward[BOND];
    // Add conversion outputs
    coinbaseTx.vout.insert(coinbaseTx.vout.end(), conversionOutputs.begin(), conversionOutputs.end());
    // Add input
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
    pblocktemplate->vchCoinbaseCommitment = m_chainstate.m_chainman.GenerateCoinbaseCommitment(*pblock, pindexPrev);
    pblocktemplate->vTxFeesCash[0] = -nFees[CASH];
    pblocktemplate->vTxFeesBond[0] = -nFees[BOND];

    LogPrintf("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    pblock->nNonce         = 0;
    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);

    BlockValidationState state;
    if (!TestBlockValidity(state, chainparams, m_chainstate, *pblock, pindexPrev, GetAdjustedTime, false, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, state.ToString()));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n", 0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated, 0.001 * (nTime2 - nTime1), 0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        } else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= nBlockMaxWeight) {
        return false;
    }
    if (nBlockSigOpsCost + packageSigOpsCost >= MAX_BLOCK_SIGOPS_COST) {
        return false;
    }
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - conversion validity for current block supply
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package, std::optional<CTxConversionInfo>& conversionInfo) const
{
    // First check that every tx is final
    for (CTxMemPool::txiter it : package) {
        if (!IsFinalTx(it->GetTx(), nHeight, m_lock_time_cutoff)) {
            return false;
        }
    }

    // Next check the validity of each conversion tx in the package.
    // We track changes to the total supply after each conversion.
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience
    CAmounts totalSupply = {0};
    totalSupply[CASH] = pblock->cashSupply;
    totalSupply[BOND] = pblock->bondSupply;

    for (CTxMemPool::txiter it : package) {
        conversionInfo = it->GetConversionInfo();
        if (conversionInfo) {
            if (conversionInfo.value().nDeadline && conversionInfo.value().nDeadline < (uint32_t)nHeight) {
                return false;
            }
            CAmount remainder = 0;
            if (!Consensus::IsValidConversion(totalSupply, conversionInfo.value().inputs, conversionInfo.value().minOutputs, conversionInfo.value().remainderType, remainder)) {
                return false;
            }
        }
    }
    return true;
}

CTxMemPoolConversionEntry BlockAssembler::GetConversionEntry(const CTxMemPool::txiter& iter, const CTxConversionInfo& conversionInfo) const
{
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience
    CAmounts totalSupply = {0};
    totalSupply[CASH] = pblock->cashSupply;
    totalSupply[BOND] = pblock->bondSupply;

    CAmounts inputs = conversionInfo.inputs;
    CAmounts minOutputs = conversionInfo.minOutputs;

    CAmount inputAmount;
    CAmount outputAmount;
    CAmountType inputType = 0;

    if (inputs[CASH] > minOutputs[CASH] && inputs[BOND] < minOutputs[BOND]) {
        // Converting from cash to bonds
        inputAmount = inputs[CASH] - minOutputs[CASH];
        outputAmount = minOutputs[BOND] - inputs[BOND];
        inputType = CASH;
    } else if (inputs[CASH] < minOutputs[CASH] && inputs[BOND] > minOutputs[BOND]) {
        // Converting from bonds to cash
        inputAmount = inputs[BOND] - minOutputs[BOND];
        outputAmount = minOutputs[CASH] - inputs[CASH];
        inputType = BOND;
    } else {
        // This is just for safety but will never be triggered because a conversion like this would never enter the mempool
        return CTxMemPoolConversionEntry(iter, std::numeric_limits<double>::max(), inputType);
    }

    // We sort the conversions in order of their conversion rate, popping off in order of the lowest rate,
    // so we need to adjust up the conversion rate on large conversions to make them comparable to small conversions.
    // Otherwise, we might see an invalid large conversion and incorrectly assume the small conversions that come after it are invalid too.
    CAmount convertedOutput = CalculateOutputAmount(totalSupply, inputAmount, inputType);
    CAmount outputAtConversionRate = GetConvertedAmount(totalSupply, inputAmount, inputType);
    double sizeAdjustment = (double)outputAtConversionRate / (double)convertedOutput;
    double conversionRate = sizeAdjustment * (double)outputAmount / (double)inputAmount;

    return CTxMemPoolConversionEntry(iter, conversionRate, inputType);
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblocktemplate->block.vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFeesCash.push_back(iter->GetFees()[CASH]);
    pblocktemplate->vTxFeesBond.push_back(iter->GetFees()[BOND]);
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees[CASH] += iter->GetFees()[CASH];
    nFees[BOND] += iter->GetFees()[BOND];
    inBlock.insert(iter);

    std::optional<CTxConversionInfo> conversionInfo = iter->GetConversionInfo();
    if (conversionInfo) {
        CBlock* const pblock = &pblocktemplate->block; // pointer for convenience
        CAmounts totalSupply = {0};
        totalSupply[CASH] = pblock->cashSupply;
        totalSupply[BOND] = pblock->bondSupply;
        CAmountType amountType = conversionInfo.value().remainderType;
        CAmount nAmount;
        if (Consensus::IsValidConversion(totalSupply, conversionInfo.value().inputs, conversionInfo.value().minOutputs, amountType, nAmount)) {
            // Update cash and bond supply of block we are building
            pblock->cashSupply = totalSupply[CASH];
            pblock->bondSupply = totalSupply[BOND];
            if (nAmount > 0) {
                // Include remainder output amount if non-zero
                if (IsValidDestination(conversionInfo.value().destination)) {
                    // Send remainder to provided destination
                    CScript scriptPubKey = GetScriptForDestination(conversionInfo.value().destination);
                    conversionOutputs.push_back(CTxOut(amountType, nAmount, scriptPubKey));
                } else {
                    // No destination provided. Add remainder to miner fees.
                    nFees[amountType] += nAmount;
                }
            }
        }
    }

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee rate %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

/** Add descendants of given transactions to mapModifiedTx with ancestor
 * state updated assuming given transactions are inBlock. Returns number
 * of updated descendants. */
static int UpdatePackagesForAdded(const CTxMemPool& mempool,
                                  const CTxMemPool::setEntries& alreadyAdded,
                                  indexed_modified_transaction_set& mapModifiedTx,
                                  indexed_conversion_transaction_set& invalidConversionTxCash,
                                  indexed_conversion_transaction_set& invalidConversionTxBond) EXCLUSIVE_LOCKS_REQUIRED(mempool.cs)
{
    AssertLockHeld(mempool.cs);

    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc)) {
                continue;
            }
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCostWithAncestors -= it->GetSigOpCost();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }

            conversiontxiter cashit = invalidConversionTxCash.find(desc);
            if (cashit != invalidConversionTxCash.end()) {
                invalidConversionTxCash.modify(cashit, update_for_parent_inclusion(it));
            }

            conversiontxiter bondit = invalidConversionTxBond.find(desc);
            if (bondit != invalidConversionTxBond.end()) {
                invalidConversionTxBond.modify(bondit, update_for_parent_inclusion(it));
            }
        }
    }
    return nDescendantsUpdated;
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(const CTxMemPool& mempool, int& nPackagesSelected, int& nDescendantsUpdated)
{
    AssertLockHeld(mempool.cs);

    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Keep track of entries that failed due to an attempt to convert at an invalid rate.
    // invalidConversionTxCash keeps track of failed cash->bond conversions, and
    // invalidConversionTxBond keeps track of failed bond->cash conversions.
    // Both are sorted in order of their respective conversion rate, adjusted upwards for size.
    //
    // NOTE: Entries with more than one conversion in ancestor list are NOT included
    indexed_conversion_transaction_set invalidConversionTxCash;
    indexed_conversion_transaction_set invalidConversionTxBond;

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != mempool.mapTx.get<ancestor_score>().end() || !mapModifiedTx.empty()) {
        // First try to find a new transaction in mapTx to evaluate.
        //
        // Skip entries in mapTx that are already in a block or are present
        // in mapModifiedTx (which implies that the mapTx ancestor state is
        // stale due to ancestor inclusion in the block)
        // Also skip transactions that we've already failed to add. This can happen if
        // we consider a transaction in mapModifiedTx and it fails: we can then
        // potentially consider it again while walking mapTx.  It's currently
        // guaranteed to fail again, but as a belt-and-suspenders check we put it in
        // failedTx and avoid re-evaluation, since the re-evaluation would be using
        // cached size/sigops/fee values that are not actually correct.
        /** Return true if given transaction from mapTx has already been evaluated,
         * or if the transaction's cached data in mapTx is incorrect. */
        if (mi != mempool.mapTx.get<ancestor_score>().end()) {
            auto it = mempool.mapTx.project<0>(mi);
            assert(it != mempool.mapTx.end());
            if (mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it)) {
                ++mi;
                continue;
            }
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                    CompareTxMemPoolEntryByAncestorFee()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOpsCost)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                    nBlockMaxWeight - 4000) {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final, conversions are valid, and conversion deadlines haven't expired
        std::optional<CTxConversionInfo> conversionInfo;
        if (!TestPackageTransactions(ancestors, conversionInfo)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            // Set conversionInfo to null if there is more than one conversion
            // in the package
            if (conversionInfo) {
                bool seen_conversion = false;
                for (CTxMemPool::txiter it : ancestors) {
                    if (it->GetConversionInfo()) {
                        if (seen_conversion) {
                            conversionInfo = std::nullopt;
                            break;
                        } else {
                            seen_conversion = true;
                        }
                    }
                }
            }

            // Create conversion entry and add it to the list of transactions
            // that failed due an invalid conversion
            if (conversionInfo) {
                CTxMemPoolConversionEntry conversionEntry = GetConversionEntry(iter, conversionInfo.value());
                if (conversionEntry.GetConversionType() == CASH) {
                    invalidConversionTxCash.insert(conversionEntry);
                } else if (conversionEntry.GetConversionType() == BOND) {
                    invalidConversionTxBond.insert(conversionEntry);
                }
            }

            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);

        for (size_t i = 0; i < sortedEntries.size(); ++i) {
            AddToBlock(sortedEntries[i]);
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(mempool, ancestors, mapModifiedTx, invalidConversionTxCash, invalidConversionTxBond);

        if (conversionInfo) {
            // Conversion rate changed. Check if any transactions dependent upon
            // a previously invalid conversion can now be executed.
            conversionrateiter cashit = invalidConversionTxCash.get<index_by_conversion_rate>().begin();
            conversionrateiter bondit = invalidConversionTxBond.get<index_by_conversion_rate>().begin();

            // Keep track of entries that failed inclusion despite being valid, to avoid duplicate work
            CTxMemPool::setEntries failedValidConversion;
            // Keep track of entries that succeeded so we can remove them from the set when we're done
            CTxMemPool::setEntries successfulTx;

            while (
                cashit != invalidConversionTxCash.get<index_by_conversion_rate>().end() ||
                bondit != invalidConversionTxBond.get<index_by_conversion_rate>().end()
            ) {
                conversionrateiter conversionit;
                CAmountType conversionType;
                if (cashit == invalidConversionTxCash.get<index_by_conversion_rate>().end()) {
                    // We're out of entries in the cash conversion list. Set as bond entry.
                    conversionit = bondit;
                    conversionType = BOND;
                }
                else if (bondit == invalidConversionTxBond.get<index_by_conversion_rate>().end()) {
                    // We're out of entries in the bond conversion list. Set as cash entry.
                    conversionit = cashit;
                    conversionType = CASH;
                }
                else if (CompareTxMemPoolEntryByAncestorFee()(*cashit, *bondit)) {
                    // The cash entry has a higher score than the bond entry
                    conversionit = cashit;
                    conversionType = CASH;
                }
                else {
                    // The bond entry has a higher score than the cash entry
                    conversionit = bondit;
                    conversionType = BOND;
                }
                iter = conversionit->iter;

                // Skip if tx is already in the block or if it failed despite being a valid conversion
                if (inBlock.count(iter) || failedValidConversion.count(iter)) {
                    if (conversionType == CASH) {
                        ++cashit;
                    } else {
                        ++bondit;
                    }
                    continue;
                }

                CTxMemPool::setEntries ancestors;
                uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
                std::string dummy;
                mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

                onlyUnconfirmed(ancestors);
                ancestors.insert(iter);

                // Test if conversion in package is valid (will not fail for any other reason)
                std::optional<CTxConversionInfo> dummyConversionInfo;
                if (!TestPackageTransactions(ancestors, dummyConversionInfo)) {
                    // Conversion is not valid, so assume all other conversions of this type are not either
                    if (conversionType == CASH) {
                        // Skip to end of iterator
                        cashit = invalidConversionTxCash.get<index_by_conversion_rate>().end();
                    } else {
                        // Skip to end of iterator
                        bondit = invalidConversionTxBond.get<index_by_conversion_rate>().end();
                    }
                    continue;
                }

                // Use values modified for parent inclusion
                uint64_t packageSize = conversionit->nSizeWithAncestors;
                CAmount packageFees = conversionit->nModFeesWithAncestors;
                int64_t packageSigOpsCost = conversionit->nSigOpCostWithAncestors;

                if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
                    // Transactions after this may have better fee, so skip and continue.
                    failedValidConversion.insert(iter);
                    continue;
                }

                if (!TestPackage(packageSize, packageSigOpsCost)) {
                    failedValidConversion.insert(iter);

                    ++nConsecutiveFailed;

                    if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                            nBlockMaxWeight - 4000) {
                        // Give up if we're close to full and haven't succeeded in a while
                        break;
                    }
                    continue;
                }

                // This transaction will make it in; reset the failed counter.
                nConsecutiveFailed = 0;

                // Package can be added. Sort the entries in a valid order.
                std::vector<CTxMemPool::txiter> sortedEntries;
                SortForBlock(ancestors, sortedEntries);

                for (size_t i = 0; i < sortedEntries.size(); ++i) {
                    AddToBlock(sortedEntries[i]);
                    // Erase from the modified set, if present
                    mapModifiedTx.erase(sortedEntries[i]);
                    // Add to set of successful transactions to be erased later
                    successfulTx.insert(sortedEntries[i]);
                }

                ++nPackagesSelected;

                // Update transactions that depend on each of these
                nDescendantsUpdated += UpdatePackagesForAdded(mempool, ancestors, mapModifiedTx, invalidConversionTxCash, invalidConversionTxBond);
            }

            // Erase failed valid conversions from their respective set
            for (CTxMemPool::txiter it : failedValidConversion) {
                invalidConversionTxCash.erase(it);
                invalidConversionTxBond.erase(it);
            }

            // Erase successful transactions from the set, if present
            for (CTxMemPool::txiter it : successfulTx) {
                invalidConversionTxCash.erase(it);
                invalidConversionTxBond.erase(it);
            }
        }
    }
}

//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//

static std::vector<std::thread> minerThreads;
static std::atomic<bool> fRequestStopMining(false);

//
// ScanHash scans nonces looking for a hash with at least some zero bits.
// The nonce is usually preserved between calls, but periodically or if the
// nonce is 0xffff0000 or above, the block is rebuilt and nNonce starts over at
// zero.
//
bool static ScanHash(const CBlockHeader *pblock, uint32_t& nNonce, uint256& phash)
{
    // Initialize a blake3_hasher in the default hashing mode.
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    // Write the first 92 bytes of the block header to a blake3_hasher state.
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << *pblock;
    assert(ss.size() == 96);
    blake3_hasher_update(&hasher, (unsigned char*)&ss[0], 92);

    while (true) {
        nNonce++;

        // Write the last 4 bytes of the block header (the nonce) to a copy of
        // the blake3_hasher state, and compute the result.
        blake3_hasher hasherCopy = hasher;
        blake3_hasher_update(&hasherCopy, (unsigned char*)&nNonce, 4);
        blake3_hasher_finalize(&hasherCopy, (unsigned char*)&phash, BLAKE3_OUT_LEN);

        // Return the nonce if the hash has at least some zero bits,
        // caller will check if it has enough to reach the target
        if (((uint16_t*)&phash)[15] == 0)
            return true;

        // If nothing found after trying for a while, return -1
        if ((nNonce & 0xfff) == 0)
            return false;

        // Check for shutdown or stop request
        if (ShutdownRequested() || fRequestStopMining)
            return false;
    }
}

static bool ProcessBlockFound(ChainstateManager* chainman, const CBlock* pblock)
{
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s cash (unscaled)\n", FormatMoney(pblock->vtx[0]->vout[CASH].nValue));
    LogPrintf("generated %s bonds (unscaled)\n", FormatMoney(pblock->vtx[0]->vout[BOND].nValue));

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainman->ActiveTip()->GetBlockHash())
            return error("BitcoinMiner: generated block is stale");
    }

    // Process this block the same as if we had received it from another node
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
    if (!chainman->ProcessNewBlock(shared_pblock, /*force_processing=*/true, /*min_pow_checked=*/true, nullptr))
        return error("BitcoinMiner: ProcessNewBlock, block not accepted");

    return true;
}

void static BitcoinMiner(ChainstateManager* chainman, CConnman* connman, CWallet* pwallet)
{
    LogPrintf("BitcoinMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    util::ThreadRename("bitcoin-miner");

    unsigned int nExtraNonce = 0;

    CScript coinbaseScript = CScript();
    std::shared_ptr<CReserveDestination> reserveDest;
    if (pwallet) {
        interfaces::Chain::Notifications* notificaton = pwallet;
        notificaton->reserveDestinationForMining(reserveDest);
    }
    else
        // Wallet not explicitly provided. Scan for any registered wallets.
        GetMainSignals().ReserveDestinationForMining(reserveDest);

    try {
        // Throw an error if no script was provided.  This can happen
        // due to some internal error but also if the keypool is empty.
        if (!reserveDest || !reserveDest->GetReservedDestination(true))
            throw std::runtime_error("No coinbase script available (mining requires a wallet)");

        CTxDestination dest = reserveDest->GetReservedDestination(true).value();
        CScript coinbaseScript = GetScriptForDestination(dest);

        while (true) {
            if (chainman->GetParams().MiningRequiresPeers()) {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                do {
                    if (connman->GetNodeCount(ConnectionDirection::Both) > 0 && !chainman->ActiveChainstate().IsInitialBlockDownload())
                        break;
                    UninterruptibleSleep(std::chrono::milliseconds{1000});
                } while (true);
            }

            //
            // Create new block
            //
            CBlockIndex* pindexPrev;
            {
                LOCK(cs_main);
                pindexPrev = chainman->ActiveTip();
            }
            CTxMemPool* mempool = chainman->ActiveChainstate().GetMempool();
            unsigned int nTransactionsUpdatedLast = mempool->GetTransactionsUpdated();

            std::unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler{chainman->ActiveChainstate(), mempool}.CreateNewBlock(coinbaseScript));
            if (!pblocktemplate.get())
            {
                LogPrintf("Error in BitcoinMiner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                return;
            }
            CBlock *pblock = &pblocktemplate->block;
            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

            LogPrintf("Running BitcoinMiner with %u transactions in block (%u block weight)\n", pblock->vtx.size(),
                GetBlockWeight(*pblock));

            //
            // Search
            //
            int64_t nStart = GetTime();
            arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);
            uint256 hash;
            uint32_t nNonce = 0;
            while (true) {
                // Check if something found
                if (ScanHash(pblock, nNonce, hash))
                {
                    if (UintToArith256(hash) <= hashTarget)
                    {
                        // Found a solution
                        pblock->nNonce = nNonce;
                        assert(hash == pblock->GetHash());

                        SetThreadPriority(THREAD_PRIORITY_NORMAL);
                        LogPrintf("BitcoinMiner:\n");
                        LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", hash.GetHex(), hashTarget.GetHex());
                        ProcessBlockFound(chainman, pblock);
                        SetThreadPriority(THREAD_PRIORITY_LOWEST);
                        reserveDest->KeepDestination();

                        // In regression test mode, stop mining after a block is found.
                        if (chainman->GetParams().MineBlocksOnDemand())
                            return;

                        break;
                    }
                }

                // Check for shutdown or stop request or if block needs to be rebuilt
                if (ShutdownRequested() || fRequestStopMining)
                    return;
                // Regtest mode doesn't require peers
                if (connman->GetNodeCount(ConnectionDirection::Both) == 0 && chainman->GetParams().MiningRequiresPeers())
                    break;
                if (nNonce >= 0xffff0000)
                    break;
                if (mempool->GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                    break;

                {
                    LOCK(cs_main);
                    if (pindexPrev != chainman->ActiveTip())
                        break;
                }

                // Update nTime every few seconds
                if (UpdateTime(pblock, chainman->GetParams().GetConsensus(), pindexPrev) < 0)
                    break; // Recreate the block if the clock has run backwards,
                           // so that we can use the correct time.
                if (chainman->GetParams().GetConsensus().fPowAllowMinDifficultyBlocks)
                {
                    // Changing pblock->nTime can change work required on testnet:
                    hashTarget.SetCompact(pblock->nBits);
                }
            }
        }
    }
    catch (const std::runtime_error &e)
    {
        LogPrintf("BitcoinMiner runtime error: %s\n", e.what());
        return;
    }
}

void StartMining(NodeContext& context, int nThreads, CWallet* pwallet)
{
    static std::vector<std::thread> minerThreads;

    if (nThreads < 0)
        nThreads = GetNumCores();

    StopMining();

    if (nThreads == 0)
        return;

    if (!context.chainman || !context.connman)
        return;

    ChainstateManager& chainman = *context.chainman;
    CConnman& connman = *context.connman;
    fRequestStopMining = false;

    for (int i = 0; i < nThreads; i++)
        minerThreads.push_back(std::thread(&BitcoinMiner, &chainman, &connman, pwallet));
}

void StopMining() {
    fRequestStopMining = true;
    for (auto& t: minerThreads)
        t.join();
    minerThreads.clear();
}

} // namespace node
