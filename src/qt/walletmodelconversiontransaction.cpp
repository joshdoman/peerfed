// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifdef HAVE_CONFIG_H
#include <config/bitcoin-config.h>
#endif

#include <qt/walletmodelconversiontransaction.h>

#include <policy/policy.h>

WalletModelConversionTransaction::WalletModelConversionTransaction(const CAmount _maxInput, const CAmount _minOutput, const CAmountType _inputType,
                                                                    const CAmountType _outputType, const CAmountType _remainderType) :
    maxInput(_maxInput),
    minOutput(_minOutput),
    inputType(_inputType),
    outputType(_outputType),
    remainderType(_remainderType),
    fee(0),
    feeType(0),
{
}

CTransactionRef& WalletModelTransaction::getWtx()
{
    return wtx;
}

void WalletModelTransaction::setWtx(const CTransactionRef& newTx)
{
    wtx = newTx;
}

unsigned int WalletModelTransaction::getTransactionSize()
{
    return wtx ? GetVirtualTransactionSize(*wtx) : 0;
}

CAmount WalletModelTransaction::getTransactionFee() const
{
    return fee;
}

CAmountType WalletModelTransaction::getTransactionFeeType() const
{
    return feeType;
}

void WalletModelTransaction::setTransactionFee(const CAmount& newFee, const CAmountType& newFeeType)
{
    fee = newFee;
    feeType = newFeeType;
}
