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

CAmount GetConvertedAmount(const CAmounts& totalSupply, const CAmount& amount, const CAmountType& amountType, const bool& roundedUp) {
    if (totalSupply[!amountType] == 0) {
        // Use expected output amount upon conversion
        return CalculateOutputAmount(totalSupply, amount, amountType);
    } else if (totalSupply[amountType] == 0) {
        // Use required input amount in a conversion
        return CalculateInputAmount(totalSupply, amount, amountType);
    } else {
        // Multiple amount by marginal conversion rate
        CAmount convertedAmount = ((int256_t)amount * (int256_t)totalSupply[amountType] / ((int256_t)totalSupply[!amountType])).convert_to<CAmount>();
        if (roundedUp) convertedAmount += 1;
        return convertedAmount;
    }
}
