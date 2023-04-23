// Copyright (c) 2023 Josh Doman
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>

#include <boost/multiprecision/cpp_int.hpp>

using namespace boost::multiprecision;

CAmount CalculateOutputAmount(const CAmounts& totalSupply, const CAmount& inputAmount, const CAmountType& inputType)
{
    if (inputAmount > totalSupply[inputType])
        // Input amount exceeds total supply
        return 0;

    // Calculate sum-of-squares invariant and determine new output
    int128_t invariant_sq_in = pow((int128_t)totalSupply[CASH], 2) + pow((int128_t)totalSupply[BOND], 2); // K^2
    int128_t new_input_sq = pow((int128_t)totalSupply[inputType] - (int128_t)inputAmount, 2); // (A - ΔA)^2
    CAmount new_output = sqrt(invariant_sq_in - new_input_sq).convert_to<CAmount>(); // B' = sqrt(K^2 - (A - ΔA)^2)
    return new_output - totalSupply[!inputType]; // ΔB = B' - B
}

CAmount CalculateInputAmount(const CAmounts& totalSupply, const CAmount& outputAmount, const CAmountType& outputType)
{
    // Calculate sum-of-squares invariant
    int128_t invariant_sq_in = pow((int128_t)totalSupply[CASH], 2) + pow((int128_t)totalSupply[BOND], 2); // K^2
    int128_t new_output_sq = pow((int128_t)totalSupply[outputType] + (int128_t)outputAmount, 2); // (B + ΔB)^2
    if (new_output_sq > invariant_sq_in)
        // New output amount exceeds maximum available with current total supply
        return 0;

    CAmount new_input = sqrt(invariant_sq_in - new_output_sq).convert_to<CAmount>(); // A' = sqrt(K^2 - (B + ΔB)^2)
    return totalSupply[!outputType] - new_input; // ΔA = A - A'
}

CAmount NormalizedBondAmount(const CAmounts& totalSupply, const CAmount& bondAmount) {
    if (totalSupply[CASH] == 0) {
        // Use expected output amount converting bond amount
        return CalculateOutputAmount(totalSupply, bondAmount, BOND);
    } else if (totalSupply[BOND] == 0) {
        // Use required input amount to obtain bond amount
        return CalculateInputAmount(totalSupply, bondAmount, BOND);
    } else {
        // Multiple bond amount by marginal conversion rate B / M
        return ((int256_t)bondAmount * (int256_t)totalSupply[BOND] / ((int256_t)totalSupply[CASH])).convert_to<CAmount>();
    }
}
