// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_WALLETMODELTRANSACTION_H
#define BITCOIN_QT_WALLETMODELTRANSACTION_H

#include <primitives/transaction.h>
#include <qt/sendcoinsrecipient.h>

#include <consensus/amount.h>

#include <QObject>

class SendCoinsRecipient;

namespace interfaces {
class Node;
}

/** Data model for a walletmodel transaction. */
class WalletModelTransaction
{
public:
    explicit WalletModelTransaction(const CAmountType amountType, const QList<SendCoinsRecipient> &recipients);

    CAmountType getAmountType() const;

    QList<SendCoinsRecipient> getRecipients() const;

    CTransactionRef& getWtx();
    void setWtx(const CTransactionRef&);

    unsigned int getTransactionSize();

    void setTransactionFee(const CAmount& newFee);
    CAmount getTransactionFee() const;

    CAmount getTotalTransactionAmount(const CAmountScaleFactor& scaleFactor) const; // returns unscaled total amount

    void reassignAmounts(int nChangePosRet, const CAmountScaleFactor& scaleFactor); // needed for the subtract-fee-from-amount feature

private:
    CAmountType amountType;
    QList<SendCoinsRecipient> recipients;
    CTransactionRef wtx;
    CAmount fee;
};

#endif // BITCOIN_QT_WALLETMODELTRANSACTION_H
