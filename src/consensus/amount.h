// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_AMOUNT_H
#define BITCOIN_CONSENSUS_AMOUNT_H

#include <cstdint>
#include <array>

/** Coin type (0 for 'cash', 1 for 'bond') */
typedef int8_t CAmountType;
static constexpr CAmountType CASH = 0;
static constexpr CAmountType BOND = 1;
static constexpr CAmountType UNKNOWN = 2;

/** Amount in satoshis (Can be negative) */
typedef int64_t CAmount;
typedef std::array<CAmount, 2> CAmounts;

/** The amount of satoshis in one BTC. */
static constexpr CAmount COIN = 100000000;

/** Scale factor to apply to CAmount */
typedef uint64_t CAmountScaleFactor;

/** The base scale factor (at genesis) */
static constexpr CAmountScaleFactor BASE_FACTOR = 10000000000;

/** No amount larger than this (in satoshi) is valid.
 *
 * Note that this constant is *not* the total money supply, which in Bitcoin
 * currently happens to be less than 21,000,000 BTC for various reasons, but
 * rather a sanity check. As this sanity check is used by consensus-critical
 * validation code, the exact value of the MAX_MONEY constant is consensus
 * critical; in unusual circumstances like a(nother) overflow bug that allowed
 * for the creation of coins out of thin air modification could lead to a fork.
 * */
static constexpr CAmount MAX_MONEY = 21000000 * COIN;
inline bool MoneyRange(const CAmount& nValue) { return (nValue >= 0 && nValue <= MAX_MONEY); }

CAmount ScaleAmount(const CAmount& nValue, const CAmountScaleFactor& scaleFactor);

CAmount DescaleAmount(const CAmount& scaledValue, const CAmountScaleFactor& scaleFactor);

#endif // BITCOIN_CONSENSUS_AMOUNT_H
