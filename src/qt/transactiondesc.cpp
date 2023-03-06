// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifdef HAVE_CONFIG_H
#include <config/bitcoin-config.h>
#endif

#include <qt/transactiondesc.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/paymentserver.h>
#include <qt/transactionrecord.h>

#include <consensus/consensus.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <policy/policy.h>
#include <util/system.h>
#include <validation.h>
#include <wallet/ismine.h>

#include <stdint.h>
#include <string>

#include <QLatin1String>

using wallet::ISMINE_ALL;
using wallet::ISMINE_SPENDABLE;
using wallet::ISMINE_WATCH_ONLY;
using wallet::isminetype;

QString TransactionDesc::FormatTxStatus(const interfaces::WalletTxStatus& status, bool inMempool)
{
    int depth = status.depth_in_main_chain;
    if (depth < 0) {
        /*: Text explaining the current status of a transaction, shown in the
            status field of the details window for this transaction. This status
            represents an unconfirmed transaction that conflicts with a confirmed
            transaction. */
        return tr("conflicted with a transaction with %1 confirmations").arg(-depth);
    } else if (depth == 0) {
        QString s;
        if (status.is_expired) {
            s = tr("Expired");
        } else if (inMempool) {
            /*: Text explaining the current status of a transaction, shown in the
                status field of the details window for this transaction. This status
                represents an unconfirmed transaction that is in the memory pool. */
            s = tr("0/unconfirmed, in memory pool");
        } else {
            /*: Text explaining the current status of a transaction, shown in the
                status field of the details window for this transaction. This status
                represents an unconfirmed transaction that is not in the memory pool. */
            s = tr("0/unconfirmed, not in memory pool");
        }
        if (status.is_abandoned) {
            /*: Text explaining the current status of a transaction, shown in the
                status field of the details window for this transaction. This
                status represents an abandoned transaction. */
            s += QLatin1String(", ") + tr("abandoned");
        }
        return s;
    } else if (depth < 6) {
        /*: Text explaining the current status of a transaction, shown in the
            status field of the details window for this transaction. This
            status represents a transaction confirmed in at least one block,
            but less than 6 blocks. */
        return tr("%1/unconfirmed").arg(depth);
    } else {
        /*: Text explaining the current status of a transaction, shown in the
            status field of the details window for this transaction. This status
            represents a transaction confirmed in 6 or more blocks. */
        return tr("%1 confirmations").arg(depth);
    }
}

// Takes an encoded PaymentRequest as a string and tries to find the Common Name of the X.509 certificate
// used to sign the PaymentRequest.
bool GetPaymentRequestMerchant(const std::string& pr, QString& merchant)
{
    // Search for the supported pki type strings
    if (pr.find(std::string({0x12, 0x0b}) + "x509+sha256") != std::string::npos || pr.find(std::string({0x12, 0x09}) + "x509+sha1") != std::string::npos) {
        // We want the common name of the Subject of the cert. This should be the second occurrence
        // of the bytes 0x0603550403. The first occurrence of those is the common name of the issuer.
        // After those bytes will be either 0x13 or 0x0C, then length, then either the ascii or utf8
        // string with the common name which is the merchant name
        size_t cn_pos = pr.find({0x06, 0x03, 0x55, 0x04, 0x03});
        if (cn_pos != std::string::npos) {
            cn_pos = pr.find({0x06, 0x03, 0x55, 0x04, 0x03}, cn_pos + 5);
            if (cn_pos != std::string::npos) {
                cn_pos += 5;
                if (pr[cn_pos] == 0x13 || pr[cn_pos] == 0x0c) {
                    cn_pos++; // Consume the type
                    int str_len = pr[cn_pos];
                    cn_pos++; // Consume the string length
                    merchant = QString::fromUtf8(pr.data() + cn_pos, str_len);
                    return true;
                }
            }
        }
    }
    return false;
}

QString TransactionDesc::toHTML(interfaces::Node& node, interfaces::Wallet& wallet, TransactionRecord* rec, BitcoinUnit& cashUnit, BitcoinUnit& bondUnit)
{
    int numBlocks;
    interfaces::WalletTxStatus status;
    interfaces::WalletOrderForm orderForm;
    bool inMempool;
    interfaces::WalletTx wtx = wallet.getWalletTxDetails(rec->hash, status, orderForm, inMempool, numBlocks);

    QString strHTML;

    strHTML.reserve(4000);
    strHTML += "<html><font face='verdana, arial, helvetica, sans-serif'>";

    int64_t nTime = wtx.time;
    CAmounts nCredit = wtx.credit;
    CAmounts nDebit = wtx.debit;
    CAmounts valuesOut = wtx.tx->GetValuesOut();
    CAmounts nNet = {0};
    nNet[CASH] = nCredit[CASH] - nDebit[CASH];
    nNet[BOND] = nCredit[BOND] - nDebit[BOND];
    if (!BitcoinUnits::isShare(cashUnit))
        nNet[CASH] = ScaleAmount(nNet[CASH], wtx.scale_factor);
    if (!BitcoinUnits::isShare(bondUnit))
        nNet[BOND] = ScaleAmount(nNet[BOND], wtx.scale_factor);

    strHTML += "<b>" + tr("Status") + ":</b> " + FormatTxStatus(status, inMempool);
    strHTML += "<br>";

    strHTML += "<b>" + tr("Date") + ":</b> " + (nTime ? GUIUtil::dateTimeStr(nTime) : "") + "<br>";

    //
    // From
    //
    if (wtx.is_coinbase)
    {
        strHTML += "<b>" + tr("Source") + ":</b> " + tr("Generated") + "<br>";
    }
    else if (wtx.value_map.count("from") && !wtx.value_map["from"].empty())
    {
        // Online transaction
        strHTML += "<b>" + tr("From") + ":</b> " + GUIUtil::HtmlEscape(wtx.value_map["from"]) + "<br>";
    }
    else
    {
        // Offline transaction
        if ((nNet[CASH] > 0 && nNet[BOND] >= 0) || (nNet[CASH] >= 0 && nNet[BOND] > 0))
        {
            // Credit
            CTxDestination address = DecodeDestination(rec->address);
            if (IsValidDestination(address)) {
                std::string name;
                isminetype ismine;
                if (wallet.getAddress(address, &name, &ismine, /* purpose= */ nullptr))
                {
                    strHTML += "<b>" + tr("From") + ":</b> " + tr("unknown") + "<br>";
                    strHTML += "<b>" + tr("To") + ":</b> ";
                    strHTML += GUIUtil::HtmlEscape(rec->address);
                    QString addressOwned = ismine == ISMINE_SPENDABLE ? tr("own address") : tr("watch-only");
                    if (!name.empty())
                        strHTML += " (" + addressOwned + ", " + tr("label") + ": " + GUIUtil::HtmlEscape(name) + ")";
                    else
                        strHTML += " (" + addressOwned + ")";
                    strHTML += "<br>";
                }
            }
        }
    }

    //
    // To
    //
    if (wtx.value_map.count("to") && !wtx.value_map["to"].empty())
    {
        // Online transaction
        std::string strAddress = wtx.value_map["to"];
        strHTML += "<b>" + tr("To") + ":</b> ";
        CTxDestination dest = DecodeDestination(strAddress);
        std::string name;
        if (wallet.getAddress(
                dest, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
            strHTML += GUIUtil::HtmlEscape(name) + " ";
        strHTML += GUIUtil::HtmlEscape(strAddress) + "<br>";
    }

    //
    // Amount
    //
    if (wtx.is_coinbase && nCredit[CASH] == 0 && nCredit[BOND] == 0)
    {
        //
        // Coinbase
        //
        CAmount nUnmatured = 0;
        for (const CTxOut& txout : wtx.tx->vout) {
            if (txout.amountType == rec->amountType)
                nUnmatured += wallet.getCredit(txout, ISMINE_ALL);
        }
        strHTML += "<b>" + tr("Credit") + ":</b> ";
        BitcoinUnit unit = rec->amountType == CASH ? cashUnit : bondUnit;
        if (!BitcoinUnits::isShare(unit))
            nUnmatured = ScaleAmount(nUnmatured, wtx.scale_factor);
        if (status.is_in_main_chain)
            strHTML += BitcoinUnits::formatHtmlWithUnit(unit, nUnmatured) + " (" + tr("matures in %n more block(s)", "", status.blocks_to_maturity) + ")";
        else
            strHTML += "(" + tr("not accepted") + ")";
        strHTML += "<br>";
    }
    else if ((nNet[CASH] > 0 && nNet[BOND] >= 0) || (nNet[CASH] >= 0 && nNet[BOND] > 0))
    {
        //
        // Credit
        //
        auto amountStr = nNet[CASH] > 0 ? BitcoinUnits::formatHtmlWithUnit(cashUnit, nNet[CASH]) : BitcoinUnits::formatHtmlWithUnit(bondUnit, nNet[BOND]);
        if (nNet[CASH] > 0 && nNet[BOND] > 0)
            amountStr += ", " + BitcoinUnits::formatHtmlWithUnit(bondUnit, nNet[BOND]);
        strHTML += "<b>" + tr("Credit") + ":</b> " + amountStr + "<br>";
    }
    else
    {
        isminetype fAllFromMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txin_is_mine)
        {
            if(fAllFromMe > mine) fAllFromMe = mine;
        }

        isminetype fAllToMe = ISMINE_SPENDABLE;
        for (unsigned int i = 0; i < wtx.txout_is_mine.size(); i++)
        {
            const isminetype mine = wtx.txout_is_mine[i];
            if(fAllToMe > mine) fAllToMe = mine;
        }

        if (fAllFromMe)
        {
            if(fAllFromMe & ISMINE_WATCH_ONLY)
                strHTML += "<b>" + tr("From") + ":</b> " + tr("watch-only") + "<br>";

            //
            // Debit
            //
            auto mine = wtx.txout_is_mine.begin();
            for (const CTxOut& txout : wtx.tx->vout)
            {
                // Ignore change
                isminetype toSelf = *(mine++);
                if ((toSelf == ISMINE_SPENDABLE) && (fAllFromMe == ISMINE_SPENDABLE))
                    continue;
                if (txout.scriptPubKey.IsConversionScript())
                    continue;

                if (!wtx.value_map.count("to") || wtx.value_map["to"].empty())
                {
                    // Offline transaction
                    CTxDestination address;
                    if (ExtractDestination(txout.scriptPubKey, address))
                    {
                        strHTML += "<b>" + tr("To") + ":</b> ";
                        std::string name;
                        if (wallet.getAddress(
                                address, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
                            strHTML += GUIUtil::HtmlEscape(name) + " ";
                        strHTML += GUIUtil::HtmlEscape(EncodeDestination(address));
                        if(toSelf == ISMINE_SPENDABLE)
                            strHTML += " (own address)";
                        else if(toSelf & ISMINE_WATCH_ONLY)
                            strHTML += " (watch-only)";
                        strHTML += "<br>";
                    }
                }

                BitcoinUnit unit = txout.amountType == CASH ? cashUnit : bondUnit;
                CAmount amount = BitcoinUnits::isShare(unit) ? txout.nValue : ScaleAmount(txout.nValue, wtx.scale_factor);
                strHTML += "<b>" + tr("Debit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, -amount) + "<br>";
                if(toSelf)
                    strHTML += "<b>" + tr("Credit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, amount) + "<br>";
            }

            if (fAllToMe || wtx.is_conversion)
            {
                // Payment to self or conversion

                CAmounts nConversionTxFee = {0};
                if (wtx.is_conversion) {
                    const CTxOut& txout = wtx.tx->vout[wtx.conversion_out_n];
                    nConversionTxFee[txout.amountType] = txout.nValue;
                }

                CAmounts nChange = wtx.change;
                CAmounts totalDebit = {0};
                CAmounts totalCredit = {0};
                totalDebit[CASH] = -(nDebit[CASH] - nChange[CASH]);
                totalDebit[BOND] = -(nDebit[BOND] - nChange[BOND]);
                totalCredit[CASH] = valuesOut[CASH] - nChange[CASH] - nConversionTxFee[CASH];
                totalCredit[BOND] = valuesOut[BOND] - nChange[BOND] - nConversionTxFee[BOND];
                if (!BitcoinUnits::isShare(cashUnit)) {
                    totalDebit[CASH] = ScaleAmount(totalDebit[CASH], wtx.scale_factor);
                    totalCredit[CASH] = ScaleAmount(totalCredit[CASH], wtx.scale_factor);
                }
                if (!BitcoinUnits::isShare(bondUnit)) {
                    totalDebit[BOND] = ScaleAmount(totalDebit[BOND], wtx.scale_factor);
                    totalCredit[BOND] = ScaleAmount(totalCredit[BOND], wtx.scale_factor);
                }

                auto debitAmountStr = totalDebit[CASH] < 0 ? BitcoinUnits::formatHtmlWithUnit(cashUnit, totalDebit[CASH]) : BitcoinUnits::formatHtmlWithUnit(bondUnit, totalDebit[BOND]);
                auto creditAmountStr = totalCredit[CASH] > 0 ? BitcoinUnits::formatHtmlWithUnit(cashUnit, totalCredit[CASH]) : BitcoinUnits::formatHtmlWithUnit(bondUnit, totalCredit[BOND]);
                if (totalDebit[CASH] < 0 && totalDebit[BOND] < 0) {
                    debitAmountStr += ", " + BitcoinUnits::formatHtmlWithUnit(bondUnit, totalDebit[BOND]);
                }
                if (totalCredit[CASH] > 0 && totalCredit[BOND] > 0) {
                    creditAmountStr += ", " + BitcoinUnits::formatHtmlWithUnit(bondUnit, totalCredit[BOND]);
                }

                if (fAllToMe) {
                    strHTML += "<b>" + tr("Total debit") + ":</b> " + debitAmountStr + "<br>";
                    strHTML += "<b>" + tr("Total credit") + ":</b> " + creditAmountStr + "<br>";
                } else {
                    // Conversion payment to self
                    std::string address;
                    for (unsigned int i = 0; i < wtx.txout_address.size(); i++) {
                        if (!wtx.txout_is_mine[i]) continue;
                        auto it = wtx.txout_address[i];
                        if (address.size() > 0) address += ", ";
                        address += EncodeDestination(it);
                    }

                    strHTML += "<b>" + tr("To") + ":</b> " + GUIUtil::HtmlEscape(address) + " <br>";
                    strHTML += "<b>" + tr("Debit") + ":</b> " + debitAmountStr + "<br>";
                    strHTML += "<b>" + tr("Credit") + ":</b> " + creditAmountStr + "<br>";
                }
            }

            CAmounts nTxFee = {0};
            if (wtx.is_conversion) {
                const CTxOut& txout = wtx.tx->vout[wtx.conversion_out_n];
                nTxFee[txout.amountType] = txout.nValue;
            } else {
                nTxFee[CASH] = nDebit[CASH] - valuesOut[CASH];
                nTxFee[BOND] = nDebit[BOND] - valuesOut[BOND];
            }
            if (!BitcoinUnits::isShare(cashUnit))
                nTxFee[CASH] = ScaleAmount(nTxFee[CASH], wtx.scale_factor);
            if (!BitcoinUnits::isShare(bondUnit))
                nTxFee[BOND] = ScaleAmount(nTxFee[BOND], wtx.scale_factor);


            if (nTxFee[CASH] > 0 || nTxFee[BOND] > 0) {
                auto txFeeStr = nTxFee[CASH] > 0 ? BitcoinUnits::formatHtmlWithUnit(cashUnit, -nTxFee[CASH]) : BitcoinUnits::formatHtmlWithUnit(bondUnit, -nTxFee[BOND]);
                if (nTxFee[CASH] > 0 && nTxFee[BOND] > 0) {
                    txFeeStr += ", " + BitcoinUnits::formatHtmlWithUnit(bondUnit, -nTxFee[BOND]);
                }
                strHTML += "<b>" + tr("Transaction fee") + ":</b> " + txFeeStr + "<br>";
            }
        }
        else
        {
            //
            // Mixed debit transaction
            //
            auto mine = wtx.txin_is_mine.begin();
            for (const CTxIn& txin : wtx.tx->vin) {
                if (*(mine++)) {
                    CAmountType amountType = wallet.getDebitAmountType(txin);
                    BitcoinUnit unit = amountType == CASH ? cashUnit : bondUnit;
                    CAmount amount = -wallet.getDebit(txin, ISMINE_ALL);
                    if (!BitcoinUnits::isShare(unit))
                        amount = ScaleAmount(amount, wtx.scale_factor);
                    strHTML += "<b>" + tr("Debit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, amount) + "<br>";
                }
            }
            mine = wtx.txout_is_mine.begin();
            for (const CTxOut& txout : wtx.tx->vout) {
                if (*(mine++)) {
                    BitcoinUnit unit = txout.amountType == CASH ? cashUnit : bondUnit;
                    CAmount amount = wallet.getCredit(txout, ISMINE_ALL);
                    if (!BitcoinUnits::isShare(unit))
                        amount = ScaleAmount(amount, wtx.scale_factor);
                    strHTML += "<b>" + tr("Credit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, amount) + "<br>";
                }
            }
        }
    }

    auto amountStr = (nDebit[CASH] != 0 || nCredit[CASH] != 0) ? BitcoinUnits::formatHtmlWithUnit(cashUnit, nNet[CASH], true) : BitcoinUnits::formatHtmlWithUnit(bondUnit, nNet[BOND], true);
    if ((nDebit[CASH] != 0 || nCredit[CASH] != 0) && (nDebit[BOND] != 0 || nCredit[BOND] != 0))
        amountStr += ", " + BitcoinUnits::formatHtmlWithUnit(bondUnit, nNet[BOND], true);
    strHTML += "<b>" + tr("Net amount") + ":</b> " + amountStr + "<br>";

    //
    // Message
    //
    if (wtx.value_map.count("message") && !wtx.value_map["message"].empty())
        strHTML += "<br><b>" + tr("Message") + ":</b><br>" + GUIUtil::HtmlEscape(wtx.value_map["message"], true) + "<br>";
    if (wtx.value_map.count("comment") && !wtx.value_map["comment"].empty())
        strHTML += "<br><b>" + tr("Comment") + ":</b><br>" + GUIUtil::HtmlEscape(wtx.value_map["comment"], true) + "<br>";

    strHTML += "<b>" + tr("Transaction ID") + ":</b> " + rec->getTxHash() + "<br>";
    strHTML += "<b>" + tr("Transaction total size") + ":</b> " + QString::number(wtx.tx->GetTotalSize()) + " bytes<br>";
    strHTML += "<b>" + tr("Transaction virtual size") + ":</b> " + QString::number(GetVirtualTransactionSize(*wtx.tx)) + " bytes<br>";
    strHTML += "<b>" + tr("Output index") + ":</b> " + QString::number(rec->getOutputIndex()) + "<br>";

    // Message from normal bitcoin:URI (bitcoin:123...?message=example)
    for (const std::pair<std::string, std::string>& r : orderForm) {
        if (r.first == "Message")
            strHTML += "<br><b>" + tr("Message") + ":</b><br>" + GUIUtil::HtmlEscape(r.second, true) + "<br>";

        //
        // PaymentRequest info:
        //
        if (r.first == "PaymentRequest")
        {
            QString merchant;
            if (!GetPaymentRequestMerchant(r.second, merchant)) {
                merchant.clear();
            } else {
                merchant += tr(" (Certificate was not verified)");
            }
            if (!merchant.isNull()) {
                strHTML += "<b>" + tr("Merchant") + ":</b> " + GUIUtil::HtmlEscape(merchant) + "<br>";
            }
        }
    }

    if (wtx.is_coinbase)
    {
        quint32 numBlocksToMaturity = COINBASE_MATURITY +  1;
        strHTML += "<br>" + tr("Generated coins must mature %1 blocks before they can be spent. When you generated this block, it was broadcast to the network to be added to the block chain. If it fails to get into the chain, its state will change to \"not accepted\" and it won't be spendable. This may occasionally happen if another node generates a block within a few seconds of yours.").arg(QString::number(numBlocksToMaturity)) + "<br>";
    }

    //
    // Debug view
    //
    if (node.getLogCategories() != BCLog::NONE)
    {
        strHTML += "<hr><br>" + tr("Debug information") + "<br><br>";
        for (const CTxIn& txin : wtx.tx->vin) {
            if(wallet.txinIsMine(txin)) {
                CAmountType amountType = wallet.getDebitAmountType(txin);
                BitcoinUnit unit = amountType == CASH ? cashUnit : bondUnit;
                CAmount amount = -wallet.getDebit(txin, ISMINE_ALL);
                if (!BitcoinUnits::isShare(unit))
                    amount = ScaleAmount(amount, wtx.scale_factor);
                strHTML += "<b>" + tr("Debit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, amount) + "<br>";
            }
        }
        for (const CTxOut& txout : wtx.tx->vout) {
            if(wallet.txoutIsMine(txout)) {
                BitcoinUnit unit = txout.amountType == CASH ? cashUnit : bondUnit;
                CAmount amount = wallet.getCredit(txout, ISMINE_ALL);
                if (!BitcoinUnits::isShare(unit))
                    amount = ScaleAmount(amount, wtx.scale_factor);
                strHTML += "<b>" + tr("Credit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, amount) + "<br>";
            }
        }

        strHTML += "<br><b>" + tr("Transaction") + ":</b><br>";
        strHTML += GUIUtil::HtmlEscape(wtx.tx->ToString(), true);

        strHTML += "<br><b>" + tr("Inputs") + ":</b>";
        strHTML += "<ul>";

        for (const CTxIn& txin : wtx.tx->vin)
        {
            COutPoint prevout = txin.prevout;

            Coin prev;
            if(node.getUnspentOutput(prevout, prev))
            {
                {
                    strHTML += "<li>";
                    const CTxOut &vout = prev.out;
                    CTxDestination address;
                    if (ExtractDestination(vout.scriptPubKey, address))
                    {
                        std::string name;
                        if (wallet.getAddress(address, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
                            strHTML += GUIUtil::HtmlEscape(name) + " ";
                        strHTML += QString::fromStdString(EncodeDestination(address));
                    }
                    BitcoinUnit unit = vout.amountType == CASH ? cashUnit : bondUnit;
                    strHTML = strHTML + " " + tr("Amount") + "=" + BitcoinUnits::formatHtmlWithUnit(unit, vout.nValue);
                    strHTML = strHTML + " IsMine=" + (wallet.txoutIsMine(vout) & ISMINE_SPENDABLE ? tr("true") : tr("false")) + "</li>";
                    strHTML = strHTML + " IsWatchOnly=" + (wallet.txoutIsMine(vout) & ISMINE_WATCH_ONLY ? tr("true") : tr("false")) + "</li>";
                }
            }
        }

        strHTML += "</ul>";
    }

    strHTML += "</font></html>";
    return strHTML;
}
