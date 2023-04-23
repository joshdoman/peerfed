// Copyright (c) 2023 Josh Doman
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>

/**
 * Calculate output amount if conversion transaction is executed immediately after given block.
 * Not consensus critical.
 */
CAmount CalculateOutputAmount(const CAmounts& totalSupply, const CAmount& inputAmount, const CAmountType& inputType);

/**
 * Calculate input amount if conversion transaction is executed immediately after given block.
 * Not consensus critical.
 */
CAmount CalculateInputAmount(const CAmounts& totalSupply, const CAmount& outputAmount, const CAmountType& outputType);

/**
 * Calculate equivalent cash amount at marginal conversion rate.
 * Not consensus critical.
 */
CAmount NormalizedBondAmount(const CAmounts& totalSupply, const CAmount& bondAmount);
