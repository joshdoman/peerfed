// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_WALLETMODELCONVERSIONTRANSACTION_H
#define BITCOIN_QT_WALLETMODELCONVERSIONTRANSACTION_H

#include <primitives/transaction.h>

#include <consensus/amount.h>

#include <QObject>

/** Data model for a walletmodel conversion transaction. */
class WalletModelConversionTransaction
{
public:
    explicit WalletModelConversionTransaction(const CAmount maxInput, const CAmount minOutput, const CAmountType inputType,
                                                const CAmountType outputType, const CAmountType remainderType);

    CAmount getMaxInput();
    CAmount getMinOutput();
    CAmountType getInputType();
    CAmountType getOutputType();
    CAmountType getRemainderType();

    CTransactionRef& getWtx();
    void setWtx(const CTransactionRef&);

    unsigned int getTransactionSize();

    void setTransactionFee(const CAmount& newFee, const CAmountType& newFeeType);
    CAmount getTransactionFee() const;
    CAmountType getTransactionFeeType() const;

private:
    CAmount maxInput;
    CAmount minOutput;
    CAmountType inputType;
    CAmountType outputType;
    CAmountType remainderType;
    CTransactionRef wtx;
    CAmount fee;
    CAmountType feeType;
};

#endif // BITCOIN_QT_WALLETMODELCONVERSIONTRANSACTION_H
