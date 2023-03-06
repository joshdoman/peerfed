// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactionrecord.h>

#include <chain.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <wallet/ismine.h>

#include <stdint.h>

#include <QDateTime>

using wallet::ISMINE_SPENDABLE;
using wallet::ISMINE_WATCH_ONLY;
using wallet::isminetype;

/* Return positive answer if transaction should be shown in list.
 */
bool TransactionRecord::showTransaction()
{
    // There are currently no cases where we hide transactions, but
    // we may want to use this in the future for things like RBF.
    return true;
}

/*
 * Decompose CWallet transaction to model transaction records.
 */
QList<TransactionRecord> TransactionRecord::decomposeTransaction(const interfaces::WalletTx& wtx)
{
    QList<TransactionRecord> parts;
    int64_t nTime = wtx.time;
    CAmounts nCredit = wtx.credit;
    CAmounts nDebit = wtx.debit;
    CAmounts nNet = {0};
    nNet[CASH] = nCredit[CASH] - nDebit[CASH];
    nNet[BOND] = nCredit[BOND] - nDebit[BOND];
    CAmounts valuesOut = wtx.tx->GetValuesOut();
    uint256 hash = wtx.tx->GetHash();
    std::map<std::string, std::string> mapValue = wtx.value_map;

    if ((nNet[CASH] > 0 && nNet[BOND] >= 0) || (nNet[CASH] >= 0 && nNet[BOND] > 0) || wtx.is_coinbase)
    {
        //
        // Credit
        //
        for(unsigned int i = 0; i < wtx.tx->vout.size(); i++)
        {
            const CTxOut& txout = wtx.tx->vout[i];
            isminetype mine = wtx.txout_is_mine[i];
            if(mine)
            {
                TransactionRecord sub(hash, nTime);
                sub.idx = i; // vout index
                sub.credit = txout.nValue;
                sub.amountType = txout.amountType;
                sub.scaleFactor = wtx.scale_factor;
                sub.involvesWatchAddress = mine & ISMINE_WATCH_ONLY;
                if (wtx.txout_address_is_mine[i])
                {
                    // Received by Bitcoin Address
                    sub.type = TransactionRecord::RecvWithAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                }
                else
                {
                    // Received by IP connection (deprecated features), or a multisignature or other non-simple transaction
                    sub.type = TransactionRecord::RecvFromOther;
                    sub.address = mapValue["from"];
                }
                if (wtx.is_coinbase)
                {
                    // In a standard coinbase transaction, all non-zero outputs after the first two are conversion remainders
                    if (i < 2)
                    {
                        // Generated
                        sub.type = TransactionRecord::Generated;
                    }
                    else
                    {
                        // Converted Residual
                        sub.type = TransactionRecord::ResidualConversion;
                    }
                }

                parts.append(sub);
            }
        }
    }
    else
    {
        bool involvesWatchAddress = false;
        isminetype fAllFromMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txin_is_mine)
        {
            if(mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if(fAllFromMe > mine) fAllFromMe = mine;
        }

        isminetype fAllToMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txout_is_mine)
        {
            if(mine & ISMINE_WATCH_ONLY) involvesWatchAddress = true;
            if(fAllToMe > mine) fAllToMe = mine;
        }

        if (fAllFromMe && !fAllToMe)
        {
            //
            // Debit
            //
            CAmounts nTxFee = {0};
            if (wtx.is_conversion) {
                const CTxOut& txout = wtx.tx->vout[wtx.conversion_out_n];
                nTxFee[txout.amountType] = txout.nValue;
            } else {
                nTxFee[CASH] = nDebit[CASH] - valuesOut[CASH];
                nTxFee[BOND] = nDebit[BOND] - valuesOut[BOND];
            }

            for (unsigned int nOut = 0; nOut < wtx.tx->vout.size(); nOut++)
            {
                const CTxOut& txout = wtx.tx->vout[nOut];
                TransactionRecord sub(hash, nTime);
                sub.idx = nOut;
                sub.involvesWatchAddress = involvesWatchAddress;

                if(wtx.txout_is_mine[nOut] || txout.scriptPubKey.IsConversionScript())
                {
                    // Ignore parts sent to self, as this is usually the change
                    // from a transaction sent back to our own address.
                    //
                    // Ignore conversion output as well
                    continue;
                }

                if (!std::get_if<CNoDestination>(&wtx.txout_address[nOut]))
                {
                    // Sent to Bitcoin Address
                    sub.type = TransactionRecord::SendToAddress;
                    sub.address = EncodeDestination(wtx.txout_address[nOut]);
                }
                else
                {
                    // Sent to IP, or other non-address transaction like OP_EVAL
                    sub.type = TransactionRecord::SendToOther;
                    sub.address = mapValue["to"];
                }

                CAmount nValue = txout.nValue;
                /* Add fee to first output */
                if (nTxFee[txout.amountType] > 0)
                {
                    nValue += nTxFee[txout.amountType];
                    nTxFee[txout.amountType] = 0;
                }
                sub.debit = -nValue;
                sub.amountType = txout.amountType;
                sub.scaleFactor = wtx.scale_factor;

                parts.append(sub);
            }
        }
        // Not 'else if' because we want to show conversion amounts in a conversion transaction where an output goes to another user
        if (fAllFromMe && (fAllToMe || wtx.is_conversion))
        {
            // Payment to self
            std::string address;
            for (unsigned int i = 0; i < wtx.txout_address.size(); i++) {
                if (!wtx.txout_is_mine[i]) continue;
                auto it = wtx.txout_address[i];
                if (address.size() > 0) address += ", ";
                address += EncodeDestination(it);
            }

            CAmounts nConversionTxFee = {0};
            if (wtx.is_conversion) {
                const CTxOut& txout = wtx.tx->vout[wtx.conversion_out_n];
                nConversionTxFee[txout.amountType] = txout.nValue;
            }

            CAmounts nChange = wtx.change;
            CAmounts debit = {0};
            debit[CASH] = -(nDebit[CASH] - nChange[CASH]);
            debit[BOND] = -(nDebit[BOND] - nChange[BOND]);
            CAmounts credit = {0};
            credit[CASH] = valuesOut[CASH] - nChange[CASH] - nConversionTxFee[CASH];
            credit[BOND] = valuesOut[BOND] - nChange[BOND] - nConversionTxFee[BOND];

            auto recType = wtx.is_conversion ? TransactionRecord::Converted : TransactionRecord::SendToSelf;

            // Sort so that positive amount shows up above negative amount if two amount types are present
            if (debit[CASH] + credit[CASH] > 0) {
                parts.append(TransactionRecord(hash, nTime, recType, address, debit[CASH], credit[CASH], CASH, wtx.scale_factor));
                parts.last().involvesWatchAddress = involvesWatchAddress;   // maybe pass to TransactionRecord as constructor argument
            }
            if (debit[BOND] + credit[BOND] > 0) {
                parts.append(TransactionRecord(hash, nTime, recType, address, debit[BOND], credit[BOND], BOND, wtx.scale_factor));
                parts.last().involvesWatchAddress = involvesWatchAddress;   // maybe pass to TransactionRecord as constructor argument
            }
            if (debit[CASH] + credit[CASH] < 0) {
                parts.append(TransactionRecord(hash, nTime, recType, address, debit[CASH], credit[CASH], CASH, wtx.scale_factor));
                parts.last().involvesWatchAddress = involvesWatchAddress;   // maybe pass to TransactionRecord as constructor argument
            }
            if (debit[BOND] + credit[BOND] < 0) {
                parts.append(TransactionRecord(hash, nTime, recType, address, debit[BOND], credit[BOND], BOND, wtx.scale_factor));
                parts.last().involvesWatchAddress = involvesWatchAddress;   // maybe pass to TransactionRecord as constructor argument
            }
        }
        else if (!fAllFromMe)
        {
            //
            // Mixed debit transaction, can't break down payees
            //
            if (nNet[CASH] != 0) {
                if (nNet[CASH] < 0)
                    parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", nNet[CASH], 0, CASH, wtx.scale_factor));
                else
                    parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", 0, nNet[CASH], CASH, wtx.scale_factor));
                parts.last().involvesWatchAddress = involvesWatchAddress;
            }

            if (nNet[BOND] != 0) {
                if (nNet[BOND] < 0)
                    parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", nNet[BOND], 0, BOND, wtx.scale_factor));
                else
                    parts.append(TransactionRecord(hash, nTime, TransactionRecord::Other, "", 0, nNet[BOND], BOND, wtx.scale_factor));
                parts.last().involvesWatchAddress = involvesWatchAddress;
            }
        }
    }

    return parts;
}

void TransactionRecord::updateStatus(const interfaces::WalletTxStatus& wtx, const uint256& block_hash, int numBlocks, int64_t block_time)
{
    // Determine transaction status

    // Sort order, unrecorded transactions sort to the top
    status.sortKey = strprintf("%010d-%01d-%010u-%03d",
        wtx.block_height,
        wtx.is_coinbase ? 1 : 0,
        wtx.time_received,
        idx);
    status.countsForBalance = wtx.is_trusted && !(wtx.blocks_to_maturity > 0);
    status.depth = wtx.depth_in_main_chain;
    status.m_cur_block_hash = block_hash;

    // For generated transactions, determine maturity
    if (type == TransactionRecord::Generated || type == TransactionRecord::ResidualConversion) {
        if (wtx.blocks_to_maturity > 0)
        {
            status.status = TransactionStatus::Immature;

            if (wtx.is_in_main_chain)
            {
                status.matures_in = wtx.blocks_to_maturity;
            }
            else
            {
                status.status = TransactionStatus::NotAccepted;
            }
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    else
    {
        if (status.depth < 0)
        {
            status.status = TransactionStatus::Conflicted;
        }
        else if (status.depth == 0)
        {
            status.status = TransactionStatus::Unconfirmed;
            if (wtx.is_abandoned)
                status.status = TransactionStatus::Abandoned;
            if (wtx.is_expired)
                status.status = TransactionStatus::Expired;
        }
        else if (status.depth < RecommendedNumConfirmations)
        {
            status.status = TransactionStatus::Confirming;
        }
        else
        {
            status.status = TransactionStatus::Confirmed;
        }
    }
    status.needsUpdate = false;
}

bool TransactionRecord::statusUpdateNeeded(const uint256& block_hash) const
{
    assert(!block_hash.IsNull());
    return status.m_cur_block_hash != block_hash || status.needsUpdate;
}

QString TransactionRecord::getTxHash() const
{
    return QString::fromStdString(hash.ToString());
}

int TransactionRecord::getOutputIndex() const
{
    return idx;
}
