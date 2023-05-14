// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <crypto/blake3.h>
#include <streams.h>
#include <tinyformat.h>
#include <version.h>

uint256 CBlockHeader::GetHash() const
{
    // Initialize a blake3_hasher in the default hashing mode.
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << *this;
    blake3_hasher_update(&hasher, (unsigned char*)&ss[0], ss.size());

    // Finalize the hash. BLAKE3_OUT_LEN is the default output length, 32 bytes.
    uint256 hash;
    blake3_hasher_finalize(&hasher, (unsigned char*)&hash, BLAKE3_OUT_LEN);
    return hash;
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, cashSupply=%d, bondSupply=%d, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits,
        cashSupply,
        bondSupply,
        nNonce,
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}
