// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <wallet/receive.h>
#include <wallet/transaction.h>
#include <wallet/wallet.h>

namespace wallet {
isminetype InputIsMine(const CWallet& wallet, const CTxIn& txin)
{
    AssertLockHeld(wallet.cs_wallet);
    const CWalletTx* prev = wallet.GetWalletTx(txin.prevout.hash);
    if (prev && txin.prevout.n < prev->tx->vout.size()) {
        return wallet.IsMine(prev->tx->vout[txin.prevout.n]);
    }
    return ISMINE_NO;
}

bool AllInputsMine(const CWallet& wallet, const CTransaction& tx, const isminefilter& filter)
{
    LOCK(wallet.cs_wallet);
    for (const CTxIn& txin : tx.vin) {
        if (!(InputIsMine(wallet, txin) & filter)) return false;
    }
    return true;
}

CAmount OutputGetCredit(const CWallet& wallet, const CTxOut& txout, CAmountType amountType, const isminefilter& filter)
{
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error(std::string(__func__) + ": value out of range");
    LOCK(wallet.cs_wallet);
    return (txout.amountType == amountType && ((wallet.IsMine(txout) & filter)) ? txout.nValue : 0);
}

CAmount TxGetCredit(const CWallet& wallet, const CTransaction& tx, CAmountType amountType, const isminefilter& filter)
{
    CAmount nCredit = 0;
    for (const CTxOut& txout : tx.vout)
    {
        nCredit += OutputGetCredit(wallet, txout, amountType, filter);
        if (!MoneyRange(nCredit))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    return nCredit;
}

bool ScriptIsChange(const CWallet& wallet, const CScript& script)
{
    // TODO: fix handling of 'change' outputs. The assumption is that any
    // payment to a script that is ours, but is not in the address book
    // is change. That assumption is likely to break when we implement multisignature
    // wallets that return change back into a multi-signature-protected address;
    // a better way of identifying which outputs are 'the send' and which are
    // 'the change' will need to be implemented (maybe extend CWalletTx to remember
    // which output, if any, was change).
    AssertLockHeld(wallet.cs_wallet);
    if (wallet.IsMine(script))
    {
        CTxDestination address;
        if (!ExtractDestination(script, address))
            return true;
        if (!wallet.FindAddressBookEntry(address)) {
            return true;
        }
    }
    return false;
}

bool OutputIsChange(const CWallet& wallet, const CTxOut& txout)
{
    return ScriptIsChange(wallet, txout.scriptPubKey);
}

CAmount OutputGetChange(const CWallet& wallet, const CTxOut& txout)
{
    AssertLockHeld(wallet.cs_wallet);
    if (!MoneyRange(txout.nValue))
        throw std::runtime_error(std::string(__func__) + ": value out of range");
    return (OutputIsChange(wallet, txout) ? txout.nValue : 0);
}

CAmounts TxGetChange(const CWallet& wallet, const CWalletTx& wtx)
{
    LOCK(wallet.cs_wallet);
    CAmounts nChange = {0};
    CAmounts debit = CachedTxGetDebit(wallet, wtx, ISMINE_ALL);
    CAmounts credit = CachedTxGetCredit(wallet, wtx, ISMINE_ALL);
    for (const CTxOut& txout : wtx.tx->vout)
    {
        nChange[txout.amountType] += OutputGetChange(wallet, txout);
        if (!MoneyRange(nChange[txout.amountType]))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    if (wtx.IsConversion()) {
        // If conversion credit exceeds debit, reduce the change amount to the debit amount
        nChange[CASH] = credit[CASH] <= debit[CASH] ? nChange[CASH] : debit[CASH];
        nChange[BOND] = credit[BOND] <= debit[BOND] ? nChange[BOND] : debit[BOND];
    }
    return nChange;
}

static CAmount GetCachableAmount(const CWallet& wallet, const CWalletTx& wtx, CAmountType amountType, CWalletTx::AccountingType accountingType, const isminefilter& filter)
{
    auto& amount = wtx.m_amounts[amountType][accountingType];
    if (!amount.m_cached[filter]) {
        amount.Set(filter, accountingType == CWalletTx::DEBIT ? wallet.GetDebit(*wtx.tx, amountType, filter) : TxGetCredit(wallet, *wtx.tx, amountType, filter));
        wtx.m_is_cache_empty = false;
    }
    return amount.m_value[filter];
}

CAmounts CachedTxGetCredit(const CWallet& wallet, const CWalletTx& wtx, const isminefilter& filter)
{
    AssertLockHeld(wallet.cs_wallet);

    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (wallet.IsTxImmatureCoinBase(wtx))
        return {0};

    CAmounts credit = {0};
    const isminefilter get_amount_filter{filter & ISMINE_ALL};
    if (get_amount_filter) {
        // GetBalance can assume transactions in mapWallet won't change
        // Add both cash and bond amounts since all outputs must be of the same type
        credit[CASH] += GetCachableAmount(wallet, wtx, CASH, CWalletTx::CREDIT, get_amount_filter);
        credit[BOND] += GetCachableAmount(wallet, wtx, BOND, CWalletTx::CREDIT, get_amount_filter);
    }
    return credit;
}

CAmounts CachedTxGetDebit(const CWallet& wallet, const CWalletTx& wtx, const isminefilter& filter)
{
    if (wtx.tx->vin.empty())
        return {0};

    CAmounts debit = {0};
    const isminefilter get_amount_filter{filter & ISMINE_ALL};
    if (get_amount_filter) {
        // Add both cash and bond amounts since all inputs must be of the same type
        debit[CASH] += GetCachableAmount(wallet, wtx, CASH, CWalletTx::DEBIT, get_amount_filter);
        debit[BOND] += GetCachableAmount(wallet, wtx, BOND, CWalletTx::DEBIT, get_amount_filter);
    }
    return debit;
}

CAmounts CachedTxGetChange(const CWallet& wallet, const CWalletTx& wtx)
{
    if (wtx.fChangeCached)
        return wtx.nChangeCached;
    wtx.nChangeCached = TxGetChange(wallet, wtx);
    wtx.fChangeCached = true;
    return wtx.nChangeCached;
}

CAmount CachedTxGetImmatureCredit(const CWallet& wallet, const CWalletTx& wtx, CAmountType amountType, const isminefilter& filter)
{
    AssertLockHeld(wallet.cs_wallet);

    if (wallet.IsTxImmatureCoinBase(wtx) && wallet.IsTxInMainChain(wtx)) {
        return GetCachableAmount(wallet, wtx, amountType, CWalletTx::IMMATURE_CREDIT, filter);
    }

    return 0;
}

CAmount CachedTxGetAvailableCredit(const CWallet& wallet, const CWalletTx& wtx, CAmountType amountType, const isminefilter& filter)
{
    AssertLockHeld(wallet.cs_wallet);

    // Avoid caching ismine for NO or ALL cases (could remove this check and simplify in the future).
    bool allow_cache = (filter & ISMINE_ALL) && (filter & ISMINE_ALL) != ISMINE_ALL;

    // Must wait until coinbase is safely deep enough in the chain before valuing it
    if (wallet.IsTxImmatureCoinBase(wtx))
        return 0;

    if (allow_cache && wtx.m_amounts[amountType][CWalletTx::AVAILABLE_CREDIT].m_cached[filter]) {
        return wtx.m_amounts[amountType][CWalletTx::AVAILABLE_CREDIT].m_value[filter];
    }

    bool allow_used_addresses = (filter & ISMINE_USED) || !wallet.IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE);
    CAmount nCredit = 0;
    uint256 hashTx = wtx.GetHash();
    for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
        const CTxOut& txout = wtx.tx->vout[i];
        if (!wallet.IsSpent(COutPoint(hashTx, i)) && (allow_used_addresses || !wallet.IsSpentKey(txout.scriptPubKey))) {
            nCredit += OutputGetCredit(wallet, txout, amountType, filter);
            if (!MoneyRange(nCredit))
                throw std::runtime_error(std::string(__func__) + " : value out of range");
        }
    }

    if (allow_cache) {
        wtx.m_amounts[amountType][CWalletTx::AVAILABLE_CREDIT].Set(filter, nCredit);
        wtx.m_is_cache_empty = false;
    }

    return nCredit;
}

void CachedTxGetAmounts(const CWallet& wallet, const CWalletTx& wtx,
                  std::list<COutputEntry>& listReceived,
                  std::list<COutputEntry>& listSent,
                  std::list<COutputEntry>& listConverted,
                  CAmounts& nFee, const isminefilter& filter,
                  bool include_change)
{
    nFee = {0};
    listReceived.clear();
    listSent.clear();
    listConverted.clear();

    // Compute fee:
    CAmounts nDebit = CachedTxGetDebit(wallet, wtx, filter);
    if (nDebit[CASH] > 0 || nDebit[BOND] > 0) // debit>0 means we signed/sent this transaction
    {
        if (wtx.tx->IsConversion()) {
            std::optional<CTxOut> wrappedTxOut = wtx.tx->GetConversionOutput();
            const CTxOut& txout = wrappedTxOut.value();
            nFee[txout.amountType] = txout.nValue;
        } else {
            CAmounts nValuesOut = wtx.tx->GetValuesOut();
            nFee[CASH] = nDebit[CASH] - nValuesOut[CASH];
            nFee[BOND] = nDebit[BOND] - nValuesOut[BOND];
        }
    }

    LOCK(wallet.cs_wallet);

    if ((nDebit[CASH] > 0 || nDebit[BOND] > 0) && wtx.tx->IsConversion()) { // Conversion by us
        // Conversion.
        CAmounts nCredit = CachedTxGetCredit(wallet, wtx, filter);
        CAmounts nNet = {0};
        nNet[CASH] = nCredit[CASH] - nDebit[CASH];
        nNet[BOND] = nCredit[BOND] - nDebit[BOND];

        for (unsigned int i = 0; i < wtx.tx->vout.size(); ++i)
        {
            const CTxOut& txout = wtx.tx->vout[i];

            // Get the destination address
            CTxDestination address;

            if (!ExtractDestination(txout.scriptPubKey, address) && !txout.scriptPubKey.IsUnspendable())
            {
                wallet.WalletLogPrintf("CWalletTx::GetAmounts: Unknown transaction type found, txid %s\n",
                                        wtx.GetHash().ToString());
                address = CNoDestination();
            }

            isminetype fIsMine = wallet.IsMine(txout);
            if (fIsMine & filter)
            {
                if (nNet[txout.amountType] < 0)
                {
                    // Converted
                    COutputEntry output = {address, txout.amountType, -nNet[txout.amountType], (int)i};
                    listConverted.push_back(output);
                    nNet[txout.amountType] = 0;
                }

                if (nNet[txout.amountType] > 0)
                {
                    // Received
                    COutputEntry output = {address, txout.amountType, nNet[txout.amountType], (int)i};
                    listReceived.push_back(output);
                    nNet[txout.amountType] = 0;
                }
            }
            else if (!txout.scriptPubKey.IsConversionScript())
            {
                // Sent
                COutputEntry sOutput = {address, txout.amountType, txout.nValue, (int)i};
                listSent.push_back(sOutput);
            }
        }

        // If nNet is still negative after looking for a change output, create an output with the remaining amount, assigned to the conversion output N
        if (nNet[CASH] < 0) {
            // Converted
            COutputEntry output = {CNoDestination(), CASH, -nNet[CASH], /** must be first output */ 0};
            listConverted.push_back(output);
        }
        if (nNet[BOND] < 0) {
            // Converted
            COutputEntry output = {CNoDestination(), BOND, -nNet[BOND], /** must be first output */ 0};
            listConverted.push_back(output);
        }
    } else {
        // Sent/received.
        for (unsigned int i = 0; i < wtx.tx->vout.size(); ++i)
        {
            const CTxOut& txout = wtx.tx->vout[i];
            isminetype fIsMine = wallet.IsMine(txout);
            // Only need to handle txouts if AT LEAST one of these is true:
            //   1) they debit from us (sent)
            //   2) the output is to us (received)
            if (nDebit[CASH] > 0 || nDebit[BOND] > 0)
            {
                if (!include_change && OutputIsChange(wallet, txout))
                    continue;
            }
            else if (!(fIsMine & filter))
                continue;

            // In either case, we need to get the destination address
            CTxDestination address;

            if (!ExtractDestination(txout.scriptPubKey, address) && !txout.scriptPubKey.IsUnspendable())
            {
                wallet.WalletLogPrintf("CWalletTx::GetAmounts: Unknown transaction type found, txid %s\n",
                                        wtx.GetHash().ToString());
                address = CNoDestination();
            }

            COutputEntry output = {address, txout.amountType, txout.nValue, (int)i};

            // If we are debited by the transaction, add the output as a "sent" entry
            if (nDebit[CASH] > 0 || nDebit[BOND] > 0)
                listSent.push_back(output);

            // If we are receiving the output, add it as a "received" entry
            if (fIsMine & filter)
                listReceived.push_back(output);
        }
    }
}

bool CachedTxIsFromMe(const CWallet& wallet, const CWalletTx& wtx, const isminefilter& filter)
{
    CAmounts debit = CachedTxGetDebit(wallet, wtx, filter);
    return (debit[CASH] > 0 || debit[BOND] > 0);
}

bool CachedTxIsTrusted(const CWallet& wallet, const CWalletTx& wtx, std::set<uint256>& trusted_parents)
{
    AssertLockHeld(wallet.cs_wallet);
    int nDepth = wallet.GetTxDepthInMainChain(wtx);
    if (nDepth >= 1) return true;
    if (nDepth < 0) return false;
    // using wtx's cached debit
    if (!wallet.m_spend_zero_conf_change || !CachedTxIsFromMe(wallet, wtx, ISMINE_ALL)) return false;

    // Don't trust unconfirmed transactions from us unless they are in the mempool.
    if (!wtx.InMempool()) return false;

    // Don't trust unconfirmed conversion transactions.
    if (wtx.IsConversion()) return false;

    // Trusted if all inputs are from us and are in the mempool:
    for (const CTxIn& txin : wtx.tx->vin)
    {
        // Transactions not sent by us: not trusted
        const CWalletTx* parent = wallet.GetWalletTx(txin.prevout.hash);
        if (parent == nullptr) return false;
        const CTxOut& parentOut = parent->tx->vout[txin.prevout.n];
        // Check that this specific input being spent is trusted
        if (wallet.IsMine(parentOut) != ISMINE_SPENDABLE) return false;
        // If we've already trusted this parent, continue
        if (trusted_parents.count(parent->GetHash())) continue;
        // Recurse to check that the parent is also trusted
        if (!CachedTxIsTrusted(wallet, *parent, trusted_parents)) return false;
        trusted_parents.insert(parent->GetHash());
    }
    return true;
}

bool CachedTxIsTrusted(const CWallet& wallet, const CWalletTx& wtx)
{
    std::set<uint256> trusted_parents;
    LOCK(wallet.cs_wallet);
    return CachedTxIsTrusted(wallet, wtx, trusted_parents);
}

Balance GetBalance(const CWallet& wallet, bool amountType, const int min_depth, bool avoid_reuse)
{
    Balance ret;
    isminefilter reuse_filter = avoid_reuse ? ISMINE_NO : ISMINE_USED;
    {
        LOCK(wallet.cs_wallet);
        std::set<uint256> trusted_parents;
        for (const auto& entry : wallet.mapWallet)
        {
            const CWalletTx& wtx = entry.second;
            const bool is_trusted{CachedTxIsTrusted(wallet, wtx, trusted_parents)};
            const int tx_depth{wallet.GetTxDepthInMainChain(wtx)};
            const CAmount tx_credit_mine{CachedTxGetAvailableCredit(wallet, wtx, amountType, ISMINE_SPENDABLE | reuse_filter)};
            const CAmount tx_credit_watchonly{CachedTxGetAvailableCredit(wallet, wtx, amountType, ISMINE_WATCH_ONLY | reuse_filter)};
            if (is_trusted && tx_depth >= min_depth) {
                ret.m_mine_trusted += tx_credit_mine;
                ret.m_watchonly_trusted += tx_credit_watchonly;
            }
            if (!is_trusted && tx_depth == 0 && wtx.InMempool()) {
                ret.m_mine_untrusted_pending += tx_credit_mine;
                ret.m_watchonly_untrusted_pending += tx_credit_watchonly;
            }
            ret.m_mine_immature += CachedTxGetImmatureCredit(wallet, wtx, amountType, ISMINE_SPENDABLE);
            ret.m_watchonly_immature += CachedTxGetImmatureCredit(wallet, wtx, amountType, ISMINE_WATCH_ONLY);
        }
    }
    return ret;
}

std::map<CTxDestination, CAmount> GetAddressBalances(const CWallet& wallet)
{
    std::map<CTxDestination, CAmount> balances;

    {
        LOCK(wallet.cs_wallet);
        std::set<uint256> trusted_parents;
        for (const auto& walletEntry : wallet.mapWallet)
        {
            const CWalletTx& wtx = walletEntry.second;

            if (!CachedTxIsTrusted(wallet, wtx, trusted_parents))
                continue;

            if (wallet.IsTxImmatureCoinBase(wtx))
                continue;

            int nDepth = wallet.GetTxDepthInMainChain(wtx);
            if (nDepth < (CachedTxIsFromMe(wallet, wtx, ISMINE_ALL) ? 0 : 1))
                continue;

            for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
                const auto& output = wtx.tx->vout[i];
                CTxDestination addr;
                if (!wallet.IsMine(output))
                    continue;
                if(!ExtractDestination(output.scriptPubKey, addr))
                    continue;

                CAmount n = wallet.IsSpent(COutPoint(walletEntry.first, i)) ? 0 : output.nValue;
                balances[addr] += n;
            }
        }
    }

    return balances;
}

std::set< std::set<CTxDestination> > GetAddressGroupings(const CWallet& wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    std::set< std::set<CTxDestination> > groupings;
    std::set<CTxDestination> grouping;

    for (const auto& walletEntry : wallet.mapWallet)
    {
        const CWalletTx& wtx = walletEntry.second;

        if (wtx.tx->vin.size() > 0)
        {
            bool any_mine = false;
            // group all input addresses with each other
            for (const CTxIn& txin : wtx.tx->vin)
            {
                CTxDestination address;
                if(!InputIsMine(wallet, txin)) /* If this input isn't mine, ignore it */
                    continue;
                if(!ExtractDestination(wallet.mapWallet.at(txin.prevout.hash).tx->vout[txin.prevout.n].scriptPubKey, address))
                    continue;
                grouping.insert(address);
                any_mine = true;
            }

            // group change with input addresses
            if (any_mine)
            {
               for (const CTxOut& txout : wtx.tx->vout)
                   if (OutputIsChange(wallet, txout))
                   {
                       CTxDestination txoutAddr;
                       if(!ExtractDestination(txout.scriptPubKey, txoutAddr))
                           continue;
                       grouping.insert(txoutAddr);
                   }
            }
            if (grouping.size() > 0)
            {
                groupings.insert(grouping);
                grouping.clear();
            }
        }

        // group lone addrs by themselves
        for (const auto& txout : wtx.tx->vout)
            if (wallet.IsMine(txout))
            {
                CTxDestination address;
                if(!ExtractDestination(txout.scriptPubKey, address))
                    continue;
                grouping.insert(address);
                groupings.insert(grouping);
                grouping.clear();
            }
    }

    std::set< std::set<CTxDestination>* > uniqueGroupings; // a set of pointers to groups of addresses
    std::map< CTxDestination, std::set<CTxDestination>* > setmap;  // map addresses to the unique group containing it
    for (const std::set<CTxDestination>& _grouping : groupings)
    {
        // make a set of all the groups hit by this new group
        std::set< std::set<CTxDestination>* > hits;
        std::map< CTxDestination, std::set<CTxDestination>* >::iterator it;
        for (const CTxDestination& address : _grouping)
            if ((it = setmap.find(address)) != setmap.end())
                hits.insert((*it).second);

        // merge all hit groups into a new single group and delete old groups
        std::set<CTxDestination>* merged = new std::set<CTxDestination>(_grouping);
        for (std::set<CTxDestination>* hit : hits)
        {
            merged->insert(hit->begin(), hit->end());
            uniqueGroupings.erase(hit);
            delete hit;
        }
        uniqueGroupings.insert(merged);

        // update setmap
        for (const CTxDestination& element : *merged)
            setmap[element] = merged;
    }

    std::set< std::set<CTxDestination> > ret;
    for (const std::set<CTxDestination>* uniqueGrouping : uniqueGroupings)
    {
        ret.insert(*uniqueGrouping);
        delete uniqueGrouping;
    }

    return ret;
}
} // namespace wallet
