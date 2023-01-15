// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/transaction.h>

#include <consensus/amount.h>
#include <hash.h>
#include <script/script.h>
#include <serialize.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <version.h>

#include <cassert>
#include <stdexcept>

std::string COutPoint::ToString() const
{
    return strprintf("COutPoint(%s, %u)", hash.ToString().substr(0,10), n);
}

CTxIn::CTxIn(COutPoint prevoutIn, CScript scriptSigIn, uint32_t nSequenceIn)
{
    prevout = prevoutIn;
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

CTxIn::CTxIn(uint256 hashPrevTx, uint32_t nOut, CScript scriptSigIn, uint32_t nSequenceIn)
{
    prevout = COutPoint(hashPrevTx, nOut);
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

std::string CTxIn::ToString() const
{
    std::string str;
    str += "CTxIn(";
    str += prevout.ToString();
    if (prevout.IsNull())
        str += strprintf(", coinbase %s", HexStr(scriptSig));
    else
        str += strprintf(", scriptSig=%s", HexStr(scriptSig).substr(0, 24));
    if (nSequence != SEQUENCE_FINAL)
        str += strprintf(", nSequence=%u", nSequence);
    str += ")";
    return str;
}

CTxOut::CTxOut(const CAmount& nValueIn, CScript scriptPubKeyIn)
{
    nValue = nValueIn;
    scriptPubKey = scriptPubKeyIn;
}

CTxOut::CTxOut(CAmountType amountTypeIn, const CAmount& nValueIn, CScript scriptPubKeyIn)
{
    amountType = amountTypeIn;
    nValue = nValueIn;
    scriptPubKey = scriptPubKeyIn;
}

std::string CTxOut::ToString() const
{
    return strprintf("CTxOut(amountType=%d, nValue=%d.%08d, scriptPubKey=%s)", amountType, nValue / COIN, nValue % COIN, HexStr(scriptPubKey).substr(0, 30));
}

CMutableTransaction::CMutableTransaction() : nVersion(CTransaction::CURRENT_VERSION), nLockTime(0) {}
CMutableTransaction::CMutableTransaction(const CTransaction& tx) : vin(tx.vin), vout(tx.vout), nVersion(tx.nVersion), nLockTime(tx.nLockTime) {}

uint256 CMutableTransaction::GetHash() const
{
    return SerializeHash(*this, SER_GETHASH, SERIALIZE_TRANSACTION_NO_WITNESS);
}

uint256 CTransaction::ComputeHash() const
{
    return SerializeHash(*this, SER_GETHASH, SERIALIZE_TRANSACTION_NO_WITNESS);
}

uint256 CTransaction::ComputeWitnessHash() const
{
    if (!HasWitness()) {
        return hash;
    }
    return SerializeHash(*this, SER_GETHASH, 0);
}

CTransaction::CTransaction(const CMutableTransaction& tx) : vin(tx.vin), vout(tx.vout), nVersion(tx.nVersion), nLockTime(tx.nLockTime), hash{ComputeHash()}, m_witness_hash{ComputeWitnessHash()} {}
CTransaction::CTransaction(CMutableTransaction&& tx) : vin(std::move(tx.vin)), vout(std::move(tx.vout)), nVersion(tx.nVersion), nLockTime(tx.nLockTime), hash{ComputeHash()}, m_witness_hash{ComputeWitnessHash()} {}

CAmount CTransaction::GetValueOut() const
{
    CAmount nValueOut = 0;
    for (const auto& tx_out : vout) {
        if (!MoneyRange(tx_out.nValue) || !MoneyRange(nValueOut + tx_out.nValue))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
        nValueOut += tx_out.nValue;
    }
    assert(MoneyRange(nValueOut));
    return nValueOut;
}

CAmounts CTransaction::GetValuesOut() const
{
    CAmounts nValueOut = {0};
    for (const auto& tx_out : vout) {
        if (!MoneyRange(tx_out.nValue) || !MoneyRange(nValueOut[tx_out.amountType] + tx_out.nValue))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
        nValueOut[tx_out.amountType] += tx_out.nValue;
    }
    assert(MoneyRange(nValueOut[CASH]));
    assert(MoneyRange(nValueOut[BOND]));
    return nValueOut;
}

CAmountType CTransaction::GetAmountTypeOut() const
{
    CAmountType amountType = 0;
    bool first = true;
    for (const auto& tx_out : vout) {
        if (!first && tx_out.amountType != amountType)
            throw std::runtime_error(std::string(__func__) + ": outputs of different types");
        amountType = tx_out.amountType;
        first = false;
    }
    return amountType;
}

unsigned int CTransaction::GetTotalSize() const
{
    return ::GetSerializeSize(*this, PROTOCOL_VERSION);
}

bool CTransaction::IsConversion() const
{
    return CTransaction::GetConversionOutputN() != -1 && !CTransaction::IsCoinBase();
}

int CTransaction::GetConversionOutputN() const
{
    for (int i = 0; i < vout.size(); i++) {
        if (vout[i].scriptPubKey.IsConversionScript()) {
            return i;
        }
    }
    return -1;
}

std::string CTransaction::ToString() const
{
    std::string str;
    str += strprintf("CTransaction(hash=%s, ver=%d, vin.size=%u, vout.size=%u, nLockTime=%u)\n",
        GetHash().ToString().substr(0,10),
        nVersion,
        vin.size(),
        vout.size(),
        nLockTime);
    for (const auto& tx_in : vin)
        str += "    " + tx_in.ToString() + "\n";
    for (const auto& tx_in : vin)
        str += "    " + tx_in.scriptWitness.ToString() + "\n";
    for (const auto& tx_out : vout)
        str += "    " + tx_out.ToString() + "\n";
    return str;
}
