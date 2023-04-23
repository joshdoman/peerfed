// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/fees.h>

#include <wallet/coincontrol.h>
#include <wallet/wallet.h>


namespace wallet {
CAmount GetRequiredFee(const CWallet& wallet, unsigned int nTxBytes)
{
    return GetRequiredFeeRate(wallet).GetFee(nTxBytes);
}


CAmount GetMinimumFee(const CWallet& wallet, unsigned int nTxBytes, const CCoinControl& coin_control, FeeCalculation* feeCalc)
{
    // De-normalize minimum fee rate if fee is to be paid in bonds
    CAmount minFee = GetMinimumFeeRate(wallet, coin_control, feeCalc).GetFee(nTxBytes);
    if (coin_control.m_fee_type == BOND)
        minFee = wallet.chain().estimateConvertedAmount(minFee, CASH);
    return minFee;
}

CFeeRate GetRequiredFeeRate(const CWallet& wallet)
{
    return std::max(wallet.m_min_fee.Descaled(wallet.chain().getLastScaleFactor()), wallet.chain().relayMinFee());
}

CFeeRate GetMinimumFeeRate(const CWallet& wallet, const CCoinControl& coin_control, FeeCalculation* feeCalc)
{
    /* User control of how to calculate fee uses the following parameter precedence:
       1. coin_control.m_feerate
       2. coin_control.m_confirm_target
       3. m_pay_tx_fee (user-set member variable of wallet)
       4. m_confirm_target (user-set member variable of wallet)
       The first parameter that is set is used.
    */
    CFeeRate feerate_needed;
    if (coin_control.m_feerate) { // 1.
        feerate_needed = *(coin_control.m_feerate);
        // Descale fee rate if fee rate is scaled
        if (coin_control.fIsScaledFeeRate)
            feerate_needed = feerate_needed.Descaled(wallet.chain().getLastScaleFactor());
        if (feeCalc) feeCalc->reason = FeeReason::PAYTXFEE;
        // Allow to override automatic min/max check over coin control instance
        if (coin_control.fOverrideFeeRate) return feerate_needed;
    }
    else if (!coin_control.m_confirm_target && wallet.m_pay_tx_fee != CFeeRate(0)) { // 3. TODO: remove magic value of 0 for wallet member m_pay_tx_fee
        feerate_needed = wallet.m_pay_tx_fee.Descaled(wallet.chain().getLastScaleFactor());
        if (feeCalc) feeCalc->reason = FeeReason::PAYTXFEE;
    }
    else { // 2. or 4.
        // We will use smart fee estimation
        unsigned int target = coin_control.m_confirm_target ? *coin_control.m_confirm_target : wallet.m_confirm_target;
        // By default estimates are economical iff we are signaling opt-in-RBF
        bool conservative_estimate = !coin_control.m_signal_bip125_rbf.value_or(wallet.m_signal_rbf);
        // Allow to override the default fee estimate mode over the CoinControl instance
        if (coin_control.m_fee_mode == FeeEstimateMode::CONSERVATIVE) conservative_estimate = true;
        else if (coin_control.m_fee_mode == FeeEstimateMode::ECONOMICAL) conservative_estimate = false;

        feerate_needed = wallet.chain().estimateSmartFee(target, conservative_estimate, feeCalc);
        if (feerate_needed == CFeeRate(0)) {
            // if we don't have enough data for estimateSmartFee, then use fallback fee
            feerate_needed = wallet.m_fallback_fee.Descaled(wallet.chain().getLastScaleFactor());
            if (feeCalc) feeCalc->reason = FeeReason::FALLBACK;
            // directly return if fallback fee is disabled (feerate 0 == disabled)
            if (wallet.m_fallback_fee == CFeeRate(0)) return feerate_needed;
        }
        // Obey mempool min fee when using smart fee estimation
        CFeeRate min_mempool_feerate = wallet.chain().mempoolMinFee();
        if (feerate_needed < min_mempool_feerate) {
            feerate_needed = min_mempool_feerate;
            if (feeCalc) feeCalc->reason = FeeReason::MEMPOOL_MIN;
        }
    }

    // prevent user from paying a fee below the required fee rate
    CFeeRate required_feerate = GetRequiredFeeRate(wallet);
    // Verify that fee rate exceeds the required fee rate
    if (required_feerate > feerate_needed) {
        feerate_needed = required_feerate;
        if (feeCalc) feeCalc->reason = FeeReason::REQUIRED;
    }
    return feerate_needed;
}

CFeeRate GetDiscardRate(const CWallet& wallet, const CAmountType& amountType)
{
    unsigned int highest_target = wallet.chain().estimateMaxBlocks();
    CFeeRate wallet_discard_rate = wallet.m_discard_rate;
    CFeeRate discard_rate = wallet.chain().estimateSmartFee(highest_target, false /* conservative */);
    // Convert normalized fee rates to equivalent bond fee rates if amount is in bonds
    if (amountType == BOND) {
        wallet_discard_rate = CFeeRate(wallet.chain().estimateConvertedAmount(wallet_discard_rate.GetFeePerK(), CASH));
        discard_rate = CFeeRate(wallet.chain().estimateConvertedAmount(discard_rate.GetFeePerK(), CASH));
    }
    // Don't let discard_rate be greater than longest possible fee estimate if we get a valid fee estimate
    discard_rate = (discard_rate == CFeeRate(0)) ? wallet_discard_rate : std::min(discard_rate, wallet_discard_rate);
    // Discard rate must be at least dust relay feerate (cash and bonds have the same dust relay feerate)
    discard_rate = std::max(discard_rate, wallet.chain().relayDustFee());
    return discard_rate;
}
} // namespace wallet
