// Copyright (c) 2017-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/tx_verify.h>

#include <chain.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <util/check.h>
#include <util/moneystr.h>
#include <boost/multiprecision/cpp_int.hpp>

using namespace boost::multiprecision;

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    if (tx.nLockTime == 0)
        return true;
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;

    // Even if tx.nLockTime isn't satisfied by nBlockHeight/nBlockTime, a
    // transaction is still considered final if all inputs' nSequence ==
    // SEQUENCE_FINAL (0xffffffff), in which case nLockTime is ignored.
    //
    // Because of this behavior OP_CHECKLOCKTIMEVERIFY/CheckLockTime() will
    // also check that the spending input's nSequence != SEQUENCE_FINAL,
    // ensuring that an unsatisfied nLockTime value will actually cause
    // IsFinalTx() to return false here:
    for (const auto& txin : tx.vin) {
        if (!(txin.nSequence == CTxIn::SEQUENCE_FINAL))
            return false;
    }
    return true;
}

bool IsExpiredConversion(const CTransaction &tx, int nBlockHeight)
{
    std::optional<CTxConversionInfo> conversionInfo = GetConversionInfo(tx);
    if (conversionInfo) {
        return IsExpiredConversionInfo(conversionInfo.value(), nBlockHeight);
    } else {
        return false;
    }
}

bool IsExpiredConversionInfo(const CTxConversionInfo &conversionInfo, int nBlockHeight)
{
    return conversionInfo.nDeadline && conversionInfo.nDeadline < (uint32_t)nBlockHeight;
}

std::pair<int, int64_t> CalculateSequenceLocks(const CTransaction &tx, int flags, std::vector<int>& prevHeights, const CBlockIndex& block)
{
    assert(prevHeights.size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // tx.nVersion is signed integer so requires cast to unsigned otherwise
    // we would be doing a signed comparison and half the range of nVersion
    // wouldn't support BIP 68.
    bool fEnforceBIP68 = static_cast<uint32_t>(tx.nVersion) >= 2
                      && flags & LOCKTIME_VERIFY_SEQUENCE;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68) {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
        const CTxIn& txin = tx.vin[txinIndex];

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            // The height of this input is not relevant for sequence locks
            prevHeights[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = prevHeights[txinIndex];

        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
            const int64_t nCoinTime{Assert(block.GetAncestor(std::max(nCoinHeight - 1, 0)))->GetMedianTimePast()};
            // NOTE: Subtract 1 to maintain nLockTime semantics
            // BIP 68 relative lock times have the semantics of calculating
            // the first block or time at which the transaction would be
            // valid. When calculating the effective block time or height
            // for the entire transaction, we switch to using the
            // semantics of nLockTime which is the last invalid block
            // time or height.  Thus we subtract 1 from the calculated
            // time or height.

            // Time-based relative lock-times are measured from the
            // smallest allowed timestamp of the block containing the
            // txout being spent, which is the median time past of the
            // block prior.
            nMinTime = std::max(nMinTime, nCoinTime + (int64_t)((txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1);
        } else {
            nMinHeight = std::max(nMinHeight, nCoinHeight + (int)(txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}

bool EvaluateSequenceLocks(const CBlockIndex& block, std::pair<int, int64_t> lockPair)
{
    assert(block.pprev);
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const CTransaction &tx, int flags, std::vector<int>& prevHeights, const CBlockIndex& block)
{
    return EvaluateSequenceLocks(block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    for (const auto& txin : tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (const auto& txout : tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const CCoinsViewCache& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const CTxOut &prevout = coin.out;
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

int64_t GetTransactionSigOpCost(const CTransaction& tx, const CCoinsViewCache& inputs, uint32_t flags)
{
    int64_t nSigOps = GetLegacySigOpCount(tx) * WITNESS_SCALE_FACTOR;

    if (tx.IsCoinBase())
        return nSigOps;

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs) * WITNESS_SCALE_FACTOR;
    }

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const CTxOut &prevout = coin.out;
        nSigOps += CountWitnessSigOps(tx.vin[i].scriptSig, prevout.scriptPubKey, &tx.vin[i].scriptWitness, flags);
    }
    return nSigOps;
}

bool Consensus::CheckTxInputs(const CTransaction& tx, TxValidationState& state, const CCoinsViewCache& inputs, int nSpendHeight, CAmounts& txfees, std::optional<CTxConversionInfo>& conversionInfoRet)
{
    // are the actual inputs available?
    if (!inputs.HaveInputs(tx)) {
        return state.Invalid(TxValidationResult::TX_MISSING_INPUTS, "bad-txns-inputs-missingorspent",
                         strprintf("%s: inputs missing/spent", __func__));
    }

    CAmounts nValueIn = {0};
    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        const COutPoint &prevout = tx.vin[i].prevout;
        const Coin& coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        // If prev is coinbase, check that it's matured
        if (coin.IsCoinBase() && nSpendHeight - coin.nHeight < COINBASE_MATURITY) {
            return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND, "bad-txns-premature-spend-of-coinbase",
                strprintf("tried to spend coinbase at depth %d", nSpendHeight - coin.nHeight));
        }

        // Check for negative or overflow input values
        nValueIn[coin.out.amountType] += coin.out.nValue;
        if (!MoneyRange(coin.out.nValue) || !MoneyRange(nValueIn[CASH]) || !MoneyRange(nValueIn[BOND])) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-inputvalues-outofrange");
        }
    }

    // Check for conversion output
    conversionInfoRet = std::nullopt;
    if (tx.IsConversion()) {
        CTxConversionInfo conversionInfo = GetConversionInfo(tx).value();
        // Cache amounts in and min amounts out
        conversionInfo.inputs = nValueIn;
        conversionInfo.minOutputs = tx.GetValuesOut();
        conversionInfoRet = std::optional<CTxConversionInfo>{conversionInfo};
    }
    
    if (conversionInfoRet) {
        CTxOut conversionOut = tx.GetConversionOutput().value();
        CAmountType feeType = conversionOut.amountType;
        txfees[feeType] = conversionOut.nValue;
        txfees[!feeType] = 0;
    } else {
        CAmounts values_out = tx.GetValuesOut();
        if (nValueIn[CASH] < values_out[CASH]) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-in-belowout",
                strprintf("cash value in (%s) < cash value out (%s)", FormatMoney(nValueIn[CASH]), FormatMoney(values_out[CASH])));
        } else if (nValueIn[BOND] < values_out[BOND]) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-in-belowout",
                strprintf("bond value in (%s) < bond value out (%s)", FormatMoney(nValueIn[BOND]), FormatMoney(values_out[BOND])));
        }

        // Tally transaction fees if input and output types are the same
        CAmounts txfees_aux = {0};
        txfees_aux[CASH] = nValueIn[CASH] - values_out[CASH];
        txfees_aux[BOND] = nValueIn[BOND] - values_out[BOND];
        if (!MoneyRange(txfees_aux[CASH]) || !MoneyRange(txfees_aux[BOND])) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-fee-outofrange");
        }

        txfees = txfees_aux;
    }
    return true;
}

bool Consensus::IsValidConversion(CAmounts& totalSupply, const CAmounts inputs, const CAmounts minOutputs, const CAmountType remainderType, CAmount& remainder)
{
    // Calculate sum-of-squares invariants in and out and check that this is a valid conversion
    int128_t invariant_sq_in = pow((int128_t)totalSupply[CASH], 2) + pow((int128_t)totalSupply[BOND], 2); // K^2
    int128_t invariant_sq_min_out = pow((int128_t)(totalSupply[CASH] + minOutputs[CASH] - inputs[CASH]), 2) + pow((int128_t)(totalSupply[BOND] + minOutputs[BOND] - inputs[BOND]), 2);
    if (invariant_sq_min_out > invariant_sq_in) {
        // Invariant out cannot be greater than invariant in
        return false;
    }

    // Calculate remainder:
    // (A + ΔA + ΔA')^2 + (B + ΔB)^2 = K^2
    // (A + ΔA + ΔA')^2              = K^2 - (B + ΔB)^2
    //  A + ΔA + ΔA'                 = sqrt(K^2 - (B + ΔB)^2)
    //           ΔA'                 = sqrt(K^2 - (B + ΔB)^2) - (A + ΔA)
    remainder = (sqrt(invariant_sq_in - pow((int128_t)(totalSupply[!remainderType] + minOutputs[!remainderType] - inputs[!remainderType]), 2)) - (int128_t)(totalSupply[remainderType] + minOutputs[remainderType] - inputs[remainderType])).convert_to<CAmount>();

    // Update cash and bond supply in block header
    totalSupply[CASH] += (minOutputs[CASH] - inputs[CASH]);
    totalSupply[BOND] += (minOutputs[BOND] - inputs[BOND]);
    totalSupply[remainderType] += remainder;
    return true;
}
