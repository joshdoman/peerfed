// Copyright (c) 2023 Josh Doman
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>

#include <boost/multiprecision/cpp_int.hpp>

using namespace boost::multiprecision;

CAmount ScaleAmount(const CAmount& nValue, const CAmountScaleFactor& scaleFactor) {
    return ((int256_t)nValue * (int256_t)scaleFactor / ((int256_t)BASE_FACTOR)).convert_to<CAmount>();
}

CAmount DescaleAmount(const CAmount& scaledValue, const CAmountScaleFactor& scaleFactor) {
    CAmount baseAmount = ((int256_t)scaledValue * (int256_t)BASE_FACTOR / ((int256_t)scaleFactor)).convert_to<CAmount>();
    while (ScaleAmount(baseAmount, scaleFactor) < scaledValue) {
        baseAmount++;
    }
    return baseAmount;
}
