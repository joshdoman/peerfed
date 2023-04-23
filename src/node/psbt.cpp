// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/conversion.h>
#include <consensus/tx_verify.h>
#include <node/psbt.h>
#include <policy/policy.h>
#include <policy/settings.h>
#include <tinyformat.h>

#include <numeric>

namespace node {
PSBTAnalysis AnalyzePSBT(PartiallySignedTransaction psbtx, std::optional<CAmounts> totalSupply)
{
    // Go through each input and build status
    PSBTAnalysis result;

    bool calc_fee = true;

    CAmounts in_amt = {0};

    result.inputs.resize(psbtx.tx->vin.size());

    const PrecomputedTransactionData txdata = PrecomputePSBTData(psbtx);

    for (unsigned int i = 0; i < psbtx.tx->vin.size(); ++i) {
        PSBTInput& input = psbtx.inputs[i];
        PSBTInputAnalysis& input_analysis = result.inputs[i];

        // We set next role here and ratchet backwards as required
        input_analysis.next = PSBTRole::EXTRACTOR;

        // Check for a UTXO
        CTxOut utxo;
        if (psbtx.GetInputUTXO(utxo, i)) {
            if (!MoneyRange(utxo.nValue) || !MoneyRange(in_amt[utxo.amountType] + utxo.nValue)) {
                result.SetInvalid(strprintf("PSBT is not valid. Input %u has invalid value", i));
                return result;
            }
            in_amt[utxo.amountType] += utxo.nValue;
            input_analysis.has_utxo = true;
        } else {
            if (input.non_witness_utxo && psbtx.tx->vin[i].prevout.n >= input.non_witness_utxo->vout.size()) {
                result.SetInvalid(strprintf("PSBT is not valid. Input %u specifies invalid prevout", i));
                return result;
            }
            input_analysis.has_utxo = false;
            input_analysis.is_final = false;
            input_analysis.next = PSBTRole::UPDATER;
            calc_fee = false;
        }

        if (!utxo.IsNull() && utxo.scriptPubKey.IsUnspendable()) {
            result.SetInvalid(strprintf("PSBT is not valid. Input %u spends unspendable output", i));
            return result;
        }

        // Check if it is final
        if (!utxo.IsNull() && !PSBTInputSigned(input)) {
            input_analysis.is_final = false;

            // Figure out what is missing
            SignatureData outdata;
            bool complete = SignPSBTInput(DUMMY_SIGNING_PROVIDER, psbtx, i, &txdata, 1, &outdata);

            // Things are missing
            if (!complete) {
                input_analysis.missing_pubkeys = outdata.missing_pubkeys;
                input_analysis.missing_redeem_script = outdata.missing_redeem_script;
                input_analysis.missing_witness_script = outdata.missing_witness_script;
                input_analysis.missing_sigs = outdata.missing_sigs;

                // If we are only missing signatures and nothing else, then next is signer
                if (outdata.missing_pubkeys.empty() && outdata.missing_redeem_script.IsNull() && outdata.missing_witness_script.IsNull() && !outdata.missing_sigs.empty()) {
                    input_analysis.next = PSBTRole::SIGNER;
                } else {
                    input_analysis.next = PSBTRole::UPDATER;
                }
            } else {
                input_analysis.next = PSBTRole::FINALIZER;
            }
        } else if (!utxo.IsNull()){
            input_analysis.is_final = true;
        }
    }

    // Calculate next role for PSBT by grabbing "minimum" PSBTInput next role
    result.next = PSBTRole::EXTRACTOR;
    for (unsigned int i = 0; i < psbtx.tx->vin.size(); ++i) {
        PSBTInputAnalysis& input_analysis = result.inputs[i];
        result.next = std::min(result.next, input_analysis.next);
    }
    assert(result.next > PSBTRole::CREATOR);

    if (calc_fee) {
        // Get conversion out if one is present
        std::optional<CTxOut> conversion_out;
        for (CTxOut txout : psbtx.tx->vout) {
            if (txout.scriptPubKey.IsConversionScript()) {
                conversion_out = txout;
            }
        }
        // Get the output amount
        CAmounts out_amt = std::accumulate(psbtx.tx->vout.begin(), psbtx.tx->vout.end(), CAmounts({0}),
            [](CAmounts a, const CTxOut& b) {
                if (!MoneyRange(a[CASH]) || !MoneyRange(a[BOND]) || !MoneyRange(b.nValue) || !MoneyRange(a[b.amountType] + b.nValue)) {
                    return CAmounts({-1});
                }
                CAmounts newA = a;
                newA[b.amountType] += b.nValue;
                return newA;
            }
        );
        if (!MoneyRange(out_amt[CASH]) || !MoneyRange(out_amt[BOND])) {
            result.SetInvalid("PSBT is not valid. Output amount invalid");
            return result;
        }

        // Get the fee
        CAmounts fees = {0};
        if (conversion_out.has_value()) {
            fees[conversion_out.value().amountType] = conversion_out.value().nValue;
        } else {
            fees[CASH] = in_amt[CASH] - out_amt[CASH];
            fees[BOND] = in_amt[BOND] - out_amt[BOND];
        }
        result.fees = fees;

        // Estimate the size
        CMutableTransaction mtx(*psbtx.tx);
        CCoinsView view_dummy;
        CCoinsViewCache view(&view_dummy);
        bool success = true;

        for (unsigned int i = 0; i < psbtx.tx->vin.size(); ++i) {
            PSBTInput& input = psbtx.inputs[i];
            Coin newcoin;

            if (!SignPSBTInput(DUMMY_SIGNING_PROVIDER, psbtx, i, nullptr, 1) || !psbtx.GetInputUTXO(newcoin.out, i)) {
                success = false;
                break;
            } else {
                mtx.vin[i].scriptSig = input.final_script_sig;
                mtx.vin[i].scriptWitness = input.final_script_witness;
                newcoin.nHeight = 1;
                view.AddCoin(psbtx.tx->vin[i].prevout, std::move(newcoin), true);
            }
        }

        if (success) {
            CTransaction ctx = CTransaction(mtx);
            size_t size(GetVirtualTransactionSize(ctx, GetTransactionSigOpCost(ctx, view, STANDARD_SCRIPT_VERIFY_FLAGS), ::nBytesPerSigOp));
            result.estimated_vsize = size;
            // Estimate fee rate
            if (totalSupply) {
                CAmount normalizedFee = fees[CASH] + GetConvertedAmount(totalSupply.value(), fees[BOND], BOND);
                CFeeRate feerate(normalizedFee, size);
                result.estimated_feerate = feerate;
            }
        }

    }

    return result;
}
} // namespace node
