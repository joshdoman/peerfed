// Copyright (c) 2017-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/tx_check.h>

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <consensus/validation.h>
#include <script/standard.h>

bool CheckTransaction(const CTransaction& tx, TxValidationState& state)
{
    // Basic checks that don't depend on any context
    if (tx.vin.empty())
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-vin-empty");
    if (tx.vout.empty())
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-vout-empty");
    // Size limits (this doesn't take the witness into account, as that hasn't been checked for malleability)
    if (::GetSerializeSize(tx, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) * WITNESS_SCALE_FACTOR > MAX_BLOCK_WEIGHT)
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-oversize");

    // Check for negative or overflow output values (see CVE-2010-5139)
    CAmount nValueOut[2] = {0};
    for (const auto& txout : tx.vout)
    {
        if (txout.nValue < 0)
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-vout-negative");
        if (txout.nValue > MAX_MONEY)
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-vout-toolarge");
        nValueOut[txout.amountType] += txout.nValue;
        if (!MoneyRange(nValueOut[txout.amountType]))
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-txouttotal-toolarge");
    }

    // Check for duplicate inputs (see CVE-2018-17144)
    // While Consensus::CheckTxInputs does check if all inputs of a tx are available, and UpdateCoins marks all inputs
    // of a tx as spent, it does not check if the tx has duplicate inputs.
    // Failure to run this check will result in either a crash or an inflation bug, depending on the implementation of
    // the underlying coins database.
    std::set<COutPoint> vInOutPoints;
    for (const auto& txin : tx.vin) {
        if (!vInOutPoints.insert(txin.prevout).second)
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-inputs-duplicate");
    }

    // Check for an invalid conversion script
    if (tx.vout.size() > 0 && tx.vout[0].scriptPubKey.IsConversionScript()) {
        if (!GetConversionInfo(tx)) {
            // Unable to extract conversion info from script
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-invalid-conversion-script");
        }
    }

    // Check for an output with a conversion script that is not the first output in the transaction
    for (unsigned int i = 1; i < tx.vout.size(); i++)
    {
        CTxOut txout = tx.vout[i];
        if (txout.scriptPubKey.IsConversionScript()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-conversion-vout-not-first");
        }
    }

    if (tx.IsCoinBase())
    {
        if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 100)
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-cb-length");
        if (tx.IsConversion())
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-cb-contains-conversion-vout");
    }
    else
    {
        for (const auto& txin : tx.vin)
            if (txin.prevout.IsNull())
                return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-prevout-null");
    }

    return true;
}

bool CheckTransactionContainsOutputs(const CTransaction& tx, std::vector<CTxOut> outputs, std::string& addressWithIncorrectAmount)
{
    // Create mapping of output string to # of occurrences of output in transaction
    std::map<std::string, CAmount> addressToActualCashAmount;
    std::map<std::string, CAmount> addressToActualBondAmount;
    for (const auto& txout : tx.vout)
    {
        if (txout.amountType == CASH) {
            addressToActualCashAmount[HexStr(txout.scriptPubKey)] += txout.nValue;
        } else if (txout.amountType == BOND) {
            addressToActualBondAmount[HexStr(txout.scriptPubKey)] += txout.nValue;
        }
    }

    // Create mapping of output string to # of occurrences in outputs list
    std::map<std::string, CAmount> addressToExpectedCashAmount;
    std::map<std::string, CAmount> addressToExpectedBondAmount;
    for (const auto& output : outputs)
    {
        if (output.amountType == CASH) {
            addressToExpectedCashAmount[HexStr(output.scriptPubKey)] += output.nValue;
        } else if (output.amountType == BOND) {
            addressToExpectedBondAmount[HexStr(output.scriptPubKey)] += output.nValue;
        }
    }

    // Check that every scriptPubKey receives correct cash amount
    for (const auto& [hexStr, cashAmount] : addressToExpectedCashAmount)
    {
        if (addressToExpectedCashAmount[hexStr] != addressToActualCashAmount[hexStr]) {
            addressWithIncorrectAmount = hexStr;
            return false;
        }
    }
    // Check that every scriptPubKey receives correct cash amount
    for (const auto& [hexStr, bondAmount] : addressToExpectedBondAmount)
    {
        if (addressToExpectedBondAmount[hexStr] != addressToActualBondAmount[hexStr]) {
            addressWithIncorrectAmount = hexStr;
            return false;
        }
    }
    return true;
}
