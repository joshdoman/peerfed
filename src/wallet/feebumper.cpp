// Copyright (c) 2017-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <interfaces/chain.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <util/moneystr.h>
#include <util/rbf.h>
#include <util/system.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/feebumper.h>
#include <wallet/fees.h>
#include <wallet/receive.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

namespace wallet {
//! Check whether transaction has descendant in wallet or mempool, or has been
//! mined, or conflicts with a mined transaction. Return a feebumper::Result.
static feebumper::Result PreconditionChecks(const CWallet& wallet, const CWalletTx& wtx, bool require_mine, std::vector<bilingual_str>& errors) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    if (wallet.HasWalletSpend(wtx.tx)) {
        errors.push_back(Untranslated("Transaction has descendants in the wallet"));
        return feebumper::Result::INVALID_PARAMETER;
    }

    {
        if (wallet.chain().hasDescendantsInMempool(wtx.GetHash())) {
            errors.push_back(Untranslated("Transaction has descendants in the mempool"));
            return feebumper::Result::INVALID_PARAMETER;
        }
    }

    if (wallet.GetTxDepthInMainChain(wtx) != 0) {
        errors.push_back(Untranslated("Transaction has been mined, or is conflicted with a mined transaction"));
        return feebumper::Result::WALLET_ERROR;
    }

    if (!SignalsOptInRBF(*wtx.tx)) {
        errors.push_back(Untranslated("Transaction is not BIP 125 replaceable"));
        return feebumper::Result::WALLET_ERROR;
    }

    if (wtx.mapValue.count("replaced_by_txid")) {
        errors.push_back(strprintf(Untranslated("Cannot bump transaction %s which was already bumped by transaction %s"), wtx.GetHash().ToString(), wtx.mapValue.at("replaced_by_txid")));
        return feebumper::Result::WALLET_ERROR;
    }

    if (require_mine) {
        // check that original tx consists entirely of our inputs
        // if not, we can't bump the fee, because the wallet has no way of knowing the value of the other inputs (thus the fee)
        isminefilter filter = wallet.GetLegacyScriptPubKeyMan() && wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) ? ISMINE_WATCH_ONLY : ISMINE_SPENDABLE;
        if (!AllInputsMine(wallet, *wtx.tx, filter)) {
            errors.push_back(Untranslated("Transaction contains inputs that don't belong to this wallet"));
            return feebumper::Result::WALLET_ERROR;
        }
    }

    return feebumper::Result::OK;
}

//! Check if the user provided a valid feeRate
static feebumper::Result CheckFeeRate(const CWallet& wallet, const CWalletTx& wtx, const CFeeRate& newFeerate, const int64_t maxTxSize, CAmounts old_fees, std::vector<bilingual_str>& errors)
{
    // check that fee rate is higher than mempool's minimum fee
    // (no point in bumping fee if we know that the new tx won't be accepted to the mempool)
    // This may occur if the user set fee_rate or paytxfee too low, if fallbackfee is too low, or, perhaps,
    // in a rare situation where the mempool minimum fee increased significantly since the fee estimation just a
    // moment earlier. In this case, we report an error to the user, who may adjust the fee.
    CFeeRate minMempoolFeeRate = wallet.chain().mempoolMinFee();

    if (newFeerate.GetFeePerK() < minMempoolFeeRate.GetFeePerK()) {
        errors.push_back(strprintf(
            Untranslated("New fee rate (%s) is lower than the minimum fee rate (%s) to get into the mempool -- "),
            FormatMoney(newFeerate.GetFeePerK()),
            FormatMoney(minMempoolFeeRate.GetFeePerK())));
        return feebumper::Result::WALLET_ERROR;
    }

    CAmount normalized_new_total_fee = newFeerate.GetFee(maxTxSize);

    CFeeRate incrementalRelayFee = std::max(wallet.chain().relayIncrementalFee(), CFeeRate(WALLET_INCREMENTAL_RELAY_FEE));

    // Given old total fee and transaction size, calculate the old feeRate
    const int64_t txSize = GetVirtualTransactionSize(*(wtx.tx));
    CAmount normalizedOldFee = old_fees[CASH] + wallet.chain().estimateConvertedAmount(old_fees[BOND], BOND);
    CFeeRate nNormalizedOldFeeRate(normalizedOldFee, txSize);
    // Min total fee is old fee + relay fee
    CAmount minNormalizedTotalFee = nNormalizedOldFeeRate.GetFee(maxTxSize) + incrementalRelayFee.GetFee(maxTxSize);

    if (normalized_new_total_fee < minNormalizedTotalFee) {
        errors.push_back(strprintf(Untranslated("Insufficient normalized total fee %s, must be at least %s (normalizedOldFee %s + incrementalFee %s)"),
            FormatMoney(normalized_new_total_fee), FormatMoney(minNormalizedTotalFee), FormatMoney(nNormalizedOldFeeRate.GetFee(maxTxSize)), FormatMoney(incrementalRelayFee.GetFee(maxTxSize))));
        return feebumper::Result::INVALID_PARAMETER;
    }

    CAmount requiredFee = GetRequiredFee(wallet, maxTxSize);
    if (normalized_new_total_fee < requiredFee) {
        errors.push_back(strprintf(Untranslated("Insufficient total fee (cannot be less than required fee %s on normalized basis)"),
            FormatMoney(requiredFee)));
        return feebumper::Result::INVALID_PARAMETER;
    }

    // Check that in all cases the new fee doesn't violate maxTxFee
    const CAmount max_tx_fee = wallet.GetDefaultMaxTxFee();
    if (normalized_new_total_fee > max_tx_fee) {
        errors.push_back(strprintf(Untranslated("Specified or calculated normalized fee %s is too high (cannot be higher than -maxtxfee %s on normalized basis)"),
            FormatMoney(normalized_new_total_fee), FormatMoney(max_tx_fee)));
        return feebumper::Result::WALLET_ERROR;
    }

    return feebumper::Result::OK;
}

static CFeeRate EstimateFeeRate(const CWallet& wallet, const CWalletTx& wtx, const CAmounts old_fees, const CCoinControl& coin_control)
{
    // Get the fee rate of the original transaction. This is calculated from
    // the tx fee/vsize, so it may have been rounded down. Add 1 satoshi to the
    // result.
    int64_t txSize = GetVirtualTransactionSize(*(wtx.tx));
    CAmount normalizedOldFee = old_fees[CASH] + wallet.chain().estimateConvertedAmount(old_fees[BOND], BOND);
    CFeeRate normalizedfeerate(normalizedOldFee, txSize);
    normalizedfeerate += CFeeRate(1);

    // The node has a configurable incremental relay fee. Increment the fee by
    // the minimum of that and the wallet's conservative
    // WALLET_INCREMENTAL_RELAY_FEE value to future proof against changes to
    // network wide policy for incremental relay fee that our node may not be
    // aware of. This ensures we're over the required relay fee rate
    // (Rule 4).  The replacement tx will be at least as large as the
    // original tx, so the total fee will be greater (Rule 3)
    CFeeRate node_incremental_relay_fee = wallet.chain().relayIncrementalFee();
    CFeeRate wallet_incremental_relay_fee = CFeeRate(WALLET_INCREMENTAL_RELAY_FEE);
    normalizedfeerate += std::max(node_incremental_relay_fee, wallet_incremental_relay_fee);

    // Fee rate must also be at least the wallet's GetMinimumFeeRate
    CFeeRate min_feerate(GetMinimumFeeRate(wallet, coin_control, /*feeCalc=*/nullptr));

    // Set the required fee rate for the replacement transaction in coin control.
    return std::max(normalizedfeerate, min_feerate);
}

namespace feebumper {

bool TransactionCanBeBumped(const CWallet& wallet, const uint256& txid)
{
    LOCK(wallet.cs_wallet);
    const CWalletTx* wtx = wallet.GetWalletTx(txid);
    if (wtx == nullptr) return false;
    if (wtx->isExpired()) return false;

    std::vector<bilingual_str> errors_dummy;
    feebumper::Result res = PreconditionChecks(wallet, *wtx, /* require_mine=*/ true, errors_dummy);
    return res == feebumper::Result::OK;
}

Result CreateRateBumpTransaction(CWallet& wallet, const uint256& txid, const CCoinControl& coin_control, std::vector<bilingual_str>& errors,
                                 CAmounts& old_fees, CAmounts& new_fees, CMutableTransaction& mtx, bool require_mine)
{
    // We are going to modify coin control later, copy to re-use
    CCoinControl new_coin_control(coin_control);

    LOCK(wallet.cs_wallet);
    errors.clear();
    auto it = wallet.mapWallet.find(txid);
    if (it == wallet.mapWallet.end()) {
        errors.push_back(Untranslated("Invalid or non-wallet transaction id"));
        return Result::INVALID_ADDRESS_OR_KEY;
    }
    const CWalletTx& wtx = it->second;

    // Retrieve all of the UTXOs and add them to coin control
    // While we're here, calculate the input amount
    std::map<COutPoint, Coin> coins;
    CAmounts input_values = {0};
    std::vector<CTxOut> spent_outputs;
    for (const CTxIn& txin : wtx.tx->vin) {
        coins[txin.prevout]; // Create empty map entry keyed by prevout.
    }
    wallet.chain().findCoins(coins);
    for (const CTxIn& txin : wtx.tx->vin) {
        const Coin& coin = coins.at(txin.prevout);
        if (coin.out.IsNull()) {
            errors.push_back(Untranslated(strprintf("%s:%u is already spent", txin.prevout.hash.GetHex(), txin.prevout.n)));
            return Result::MISC_ERROR;
        }
        if (wallet.IsMine(txin.prevout)) {
            new_coin_control.Select(txin.prevout);
        } else {
            new_coin_control.SelectExternal(txin.prevout, coin.out);
        }
        input_values[coin.out.amountType] += coin.out.nValue;
        spent_outputs.push_back(coin.out);
    }

    // Figure out if we need to compute the input weight, and do so if necessary
    PrecomputedTransactionData txdata;
    txdata.Init(*wtx.tx, std::move(spent_outputs), /* force=*/ true);
    for (unsigned int i = 0; i < wtx.tx->vin.size(); ++i) {
        const CTxIn& txin = wtx.tx->vin.at(i);
        const Coin& coin = coins.at(txin.prevout);

        if (new_coin_control.IsExternalSelected(txin.prevout)) {
            // For external inputs, we estimate the size using the size of this input
            int64_t input_weight = GetTransactionInputWeight(txin);
            // Because signatures can have different sizes, we need to figure out all of the
            // signature sizes and replace them with the max sized signature.
            // In order to do this, we verify the script with a special SignatureChecker which
            // will observe the signatures verified and record their sizes.
            SignatureWeights weights;
            TransactionSignatureChecker tx_checker(wtx.tx.get(), i, coin.out.amountType, coin.out.nValue, txdata, MissingDataBehavior::FAIL);
            SignatureWeightChecker size_checker(weights, tx_checker);
            VerifyScript(txin.scriptSig, coin.out.scriptPubKey, &txin.scriptWitness, STANDARD_SCRIPT_VERIFY_FLAGS, size_checker);
            // Add the difference between max and current to input_weight so that it represents the largest the input could be
            input_weight += weights.GetWeightDiffToMax();
            new_coin_control.SetInputWeight(txin.prevout, input_weight);
        }
    }

    Result result = PreconditionChecks(wallet, wtx, require_mine, errors);
    if (result != Result::OK) {
        return result;
    }

    // Conversion info
    bool isConversion = false;
    CAmount conversionFee = 0;
    CAmountType conversionFeeType = 0;
    CAmountType remainderType = 0;
    CTxDestination remainderDest;

    // Fill in recipients(and preserve a single change key if there is one)
    // While we're here, calculate the output amount
    std::vector<CRecipient> recipients;
    CAmounts output_values = {0};
    for (const auto& output : wtx.tx->vout) {
        if (output.scriptPubKey.IsConversionScript()) {
            CTxConversionInfo conversionInfo;
            if (ExtractConversionInfo(output.scriptPubKey, conversionInfo)) {
                isConversion = true;
                conversionFee = output.nValue;
                conversionFeeType = output.amountType;
                remainderType = conversionInfo.slippageType;
                remainderDest = conversionInfo.destination;
            }
        } else if (!OutputIsChange(wallet, output)) {
            CRecipient recipient = {output.scriptPubKey, output.amountType, output.nValue, false};
            recipients.push_back(recipient);
        } else {
            CTxDestination change_dest;
            ExtractDestination(output.scriptPubKey, change_dest);
            new_coin_control.destChange = change_dest;
        }
        output_values[output.amountType] += output.nValue;
    }

    if (isConversion) {
        old_fees[conversionFeeType] = conversionFee;
        old_fees[!conversionFeeType] = 0;
    } else {
        old_fees[CASH] = input_values[CASH] - output_values[CASH];
        old_fees[BOND] = input_values[BOND] - output_values[BOND];
    }

    if (coin_control.m_fee_type && coin_control.m_feerate) {
        // The user provided a feeRate argument.
        // We calculate this here to avoid compiler warning on the cs_wallet lock
        // We need to make a temporary transaction with no input witnesses as the dummy signer expects them to be empty for external inputs
        CMutableTransaction mtx{*wtx.tx};
        for (auto& txin : mtx.vin) {
            txin.scriptSig.clear();
            txin.scriptWitness.SetNull();
        }
        const int64_t maxTxSize{CalculateMaximumSignedTxSize(CTransaction(mtx), &wallet, &new_coin_control).vsize};
        Result res = CheckFeeRate(wallet, wtx, *new_coin_control.m_feerate, maxTxSize, old_fees, errors);
        if (res != Result::OK) {
            return res;
        }
    } else {
        // The user did not provide a feeRate argument
        // First, set the fee type
        if (isConversion) {
            new_coin_control.m_fee_type = conversionFeeType;
        } else {
            new_coin_control.m_fee_type = old_fees[CASH] > 0 ? CASH : BOND;
        }
        new_coin_control.m_feerate = EstimateFeeRate(wallet, wtx, old_fees, new_coin_control);
    }

    // Fill in required inputs we are double-spending(all of them)
    // N.B.: bip125 doesn't require all the inputs in the replaced transaction to be
    // used in the replacement transaction, but it's very important for wallets to make
    // sure that happens. If not, it would be possible to bump a transaction A twice to
    // A2 and A3 where A2 and A3 don't conflict (or alternatively bump A to A2 and A2
    // to A3 where A and A3 don't conflict). If both later get confirmed then the sender
    // has accidentally double paid.
    for (const auto& inputs : wtx.tx->vin) {
        new_coin_control.Select(COutPoint(inputs.prevout));
    }
    new_coin_control.m_allow_other_inputs = true;

    // We cannot source new unconfirmed inputs(bip125 rule 2)
    new_coin_control.m_min_depth = 1;

    constexpr int RANDOM_CHANGE_POSITION = -1;
    util::Result<CreatedTransactionResult> res{util::Error{}};
    if (isConversion) {
        CAmount maxInput = std::max(input_values[CASH] - output_values[CASH], input_values[BOND] - output_values[BOND]);
        CAmount minOutput = std::max(output_values[CASH] - input_values[CASH], output_values[BOND] - input_values[BOND]);
        if (maxInput < 0) {
            errors.push_back(Untranslated(strprintf("Inputs exceed outputs for both cash and bonds")));
            return Result::MISC_ERROR;
        }
        if (minOutput < 0) {
            errors.push_back(Untranslated(strprintf("Outputs exceed inputs for both cash and bonds")));
            return Result::MISC_ERROR;
        }
        if (!recipients.empty()) {
            // TODO: Implement handling of recipients in CreateConversionTransaction
            errors.push_back(Untranslated(strprintf("Fee bumping a conversion with outputs to other recipients is not yet available.")));
            return Result::MISC_ERROR;
        }
        CAmountType inputType = input_values[CASH] > output_values[CASH] ? CASH : BOND;
        CAmountType outputType = !inputType;
        bool fSubtractFeeFromInput = false;
        WalletConversionTxDetails tx_details = {
            maxInput,
            minOutput,
            inputType,
            outputType,
            remainderType,
            fSubtractFeeFromInput,
            remainderDest,
            recipients
        };
        res = CreateConversionTransaction(wallet, tx_details, RANDOM_CHANGE_POSITION, new_coin_control, false);
    } else {
        res = CreateTransaction(wallet, recipients, RANDOM_CHANGE_POSITION, new_coin_control, false);
    }
    if (!res) {
        errors.push_back(Untranslated("Unable to create transaction.") + Untranslated(" ") + util::ErrorString(res));
        return Result::WALLET_ERROR;
    }

    const auto& txr = *res;
    // Write back new fee if successful
    new_fees[txr.feeType] = txr.fee;
    new_fees[!txr.feeType] = 0;

    // Write back transaction
    mtx = CMutableTransaction(*txr.tx);

    return Result::OK;
}

bool SignTransaction(CWallet& wallet, CMutableTransaction& mtx) {
    LOCK(wallet.cs_wallet);
    return wallet.SignTransaction(mtx);
}

Result CommitTransaction(CWallet& wallet, const uint256& txid, CMutableTransaction&& mtx, std::vector<bilingual_str>& errors, uint256& bumped_txid)
{
    LOCK(wallet.cs_wallet);
    if (!errors.empty()) {
        return Result::MISC_ERROR;
    }
    auto it = txid.IsNull() ? wallet.mapWallet.end() : wallet.mapWallet.find(txid);
    if (it == wallet.mapWallet.end()) {
        errors.push_back(Untranslated("Invalid or non-wallet transaction id"));
        return Result::MISC_ERROR;
    }
    const CWalletTx& oldWtx = it->second;

    // make sure the transaction still has no descendants and hasn't been mined in the meantime
    Result result = PreconditionChecks(wallet, oldWtx, /* require_mine=*/ false, errors);
    if (result != Result::OK) {
        return result;
    }

    // commit/broadcast the tx
    CTransactionRef tx = MakeTransactionRef(std::move(mtx));
    mapValue_t mapValue = oldWtx.mapValue;
    mapValue["replaces_txid"] = oldWtx.GetHash().ToString();

    wallet.CommitTransaction(tx, std::move(mapValue), oldWtx.vOrderForm);

    // mark the original tx as bumped
    bumped_txid = tx->GetHash();
    if (!wallet.MarkReplaced(oldWtx.GetHash(), bumped_txid)) {
        errors.push_back(Untranslated("Created new bumpfee transaction but could not mark the original transaction as replaced"));
    }
    return Result::OK;
}

} // namespace feebumper
} // namespace wallet
