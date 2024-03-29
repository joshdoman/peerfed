// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/walletmodel.h>

#include <qt/addresstablemodel.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/paymentserver.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/sendcoinsdialog.h>
#include <qt/transactiontablemodel.h>

#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <consensus/tx_verify.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <psbt.h>
#include <util/system.h> // for GetBoolArg
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h> // for CRecipient

#include <stdint.h>
#include <functional>

#include <QDebug>
#include <QMessageBox>
#include <QSet>
#include <QTimer>

using interfaces::WalletConversionTxDetails;
using wallet::CCoinControl;
using wallet::CRecipient;
using wallet::DEFAULT_DISABLE_WALLET;

WalletModel::WalletModel(std::unique_ptr<interfaces::Wallet> wallet, ClientModel& client_model, const PlatformStyle *platformStyle, QObject *parent) :
    QObject(parent),
    m_wallet(std::move(wallet)),
    m_client_model(&client_model),
    m_node(client_model.node()),
    optionsModel(client_model.getOptionsModel()),
    addressTableModel(nullptr),
    transactionTableModel(nullptr),
    recentRequestsTableModel(nullptr),
    cachedEncryptionStatus(Unencrypted),
    timer(new QTimer(this))
{
    fHaveWatchOnly = m_wallet->haveWatchOnly();
    addressTableModel = new AddressTableModel(this);
    transactionTableModel = new TransactionTableModel(platformStyle, this);
    recentRequestsTableModel = new RecentRequestsTableModel(this);

    subscribeToCoreSignals();
}

WalletModel::~WalletModel()
{
    unsubscribeFromCoreSignals();
}

void WalletModel::startPollBalance()
{
    // Update the cached balance right away, so every view can make use of it,
    // so them don't need to waste resources recalculating it.
    pollBalanceChanged();

    // This timer will be fired repeatedly to update the balance
    // Since the QTimer::timeout is a private signal, it cannot be used
    // in the GUIUtil::ExceptionSafeConnect directly.
    connect(timer, &QTimer::timeout, this, &WalletModel::timerTimeout);
    GUIUtil::ExceptionSafeConnect(this, &WalletModel::timerTimeout, this, &WalletModel::pollBalanceChanged);
    timer->start(MODEL_UPDATE_DELAY);
}

void WalletModel::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!m_client_model) timer->stop();
}

void WalletModel::updateStatus()
{
    EncryptionStatus newEncryptionStatus = getEncryptionStatus();

    if(cachedEncryptionStatus != newEncryptionStatus) {
        Q_EMIT encryptionStatusChanged();
    }
}

void WalletModel::pollBalanceChanged()
{
    // Avoid recomputing wallet balances unless a TransactionChanged or
    // BlockTip notification was received.
    if (!fForceCheckBalanceChanged && m_cached_last_update_tip == getLastBlockProcessed()) return;

    // Try to get balances and return early if locks can't be acquired. This
    // avoids the GUI from getting stuck on periodical polls if the core is
    // holding the locks for a longer time - for example, during a wallet
    // rescan.
    interfaces::WalletBalances new_balances;
    uint256 block_hash;
    if (!m_wallet->tryGetBalances(new_balances, block_hash)) {
        return;
    }

    if (fForceCheckBalanceChanged || block_hash != m_cached_last_update_tip) {
        fForceCheckBalanceChanged = false;

        // Balance and number of transactions might have changed
        m_cached_last_update_tip = block_hash;

        checkBalanceChanged(new_balances);
        if(transactionTableModel)
            transactionTableModel->updateConfirmations();
    }
}

void WalletModel::checkBalanceChanged(const interfaces::WalletBalances& new_balances)
{
    if (new_balances.balanceChanged(m_cached_balances)) {
        m_cached_balances = new_balances;
        // Emit a balanceChanged event with the scaled balance
        interfaces::WalletBalances scaled_balances = getCachedBalance();
        Q_EMIT balanceChanged(scaled_balances);
    }
}

interfaces::WalletBalances WalletModel::getCachedBalance() const
{
    // Apply scale factor
    interfaces::WalletBalances balances = m_cached_balances;
    if (m_client_model && optionsModel->getShowScaledAmount(CASH))
        balances.cash = balances.cash.applyingScaleFactor(m_client_model->getBestScaleFactor());
    if (m_client_model && optionsModel->getShowScaledAmount(BOND))
        balances.bond = balances.bond.applyingScaleFactor(m_client_model->getBestScaleFactor());
    return balances;
}

void WalletModel::updateTransaction()
{
    // Balance and number of transactions might have changed
    fForceCheckBalanceChanged = true;
}

void WalletModel::updateAddressBook(const QString &address, const QString &label,
        bool isMine, const QString &purpose, int status)
{
    if(addressTableModel)
        addressTableModel->updateEntry(address, label, isMine, purpose, status);
}

void WalletModel::updateWatchOnlyFlag(bool fHaveWatchonly)
{
    fHaveWatchOnly = fHaveWatchonly;
    Q_EMIT notifyWatchonlyChanged(fHaveWatchonly);
}

bool WalletModel::validateAddress(const QString& address) const
{
    return IsValidDestinationString(address.toStdString());
}

WalletModel::SendCoinsReturn WalletModel::prepareTransaction(WalletModelTransaction &transaction, const CCoinControl& coinControl)
{
    CAmountType amountType = transaction.getAmountType();
    CAmount total = 0;
    bool fSubtractFeeFromAmount = false;
    QList<SendCoinsRecipient> recipients = transaction.getRecipients();
    std::vector<CRecipient> vecSend;

    if(recipients.empty())
    {
        return OK;
    }

    QSet<QString> setAddress; // Used to detect duplicates
    int nAddresses = 0;

    // Pre-check input data for validity
    for (const SendCoinsRecipient &rcp : recipients)
    {
        if (rcp.fSubtractFeeFromAmount)
            fSubtractFeeFromAmount = true;
        {   // User-entered bitcoin address / amount:
            if(!validateAddress(rcp.address))
            {
                return InvalidAddress;
            }
            if(rcp.amount <= 0)
            {
                return InvalidAmount;
            }
            setAddress.insert(rcp.address);
            ++nAddresses;

            CAmount amount = rcp.amount;
            if (rcp.isScaled)
                amount = DescaleAmount(amount, getBestScaleFactor());

            CScript scriptPubKey = GetScriptForDestination(DecodeDestination(rcp.address.toStdString()));
            CRecipient recipient = {scriptPubKey, amountType, amount, rcp.fSubtractFeeFromAmount};
            vecSend.push_back(recipient);

            total += amount;
        }
    }
    if(setAddress.size() != nAddresses)
    {
        return DuplicateAddress;
    }

    // If no coin was manually selected, use the cached balance
    // Future: can merge this call with 'createTransaction'.
    CAmount nBalance = getAvailableBalance(amountType, &coinControl);

    if(total > nBalance)
    {
        return AmountExceedsBalance;
    }

    {
        CAmount nFeeRequired = 0;
        int nChangePosRet = -1;

        auto& newTx = transaction.getWtx();
        const auto& res = m_wallet->createTransaction(vecSend, coinControl, !wallet().privateKeysDisabled() /* sign */, nChangePosRet, nFeeRequired);
        newTx = res ? *res : nullptr;
        transaction.setTransactionFee(nFeeRequired);
        if (fSubtractFeeFromAmount && newTx)
            transaction.reassignAmounts(nChangePosRet, getBestScaleFactor());

        if(!newTx)
        {
            if(!fSubtractFeeFromAmount && (total + nFeeRequired) > nBalance)
            {
                return SendCoinsReturn(AmountWithFeeExceedsBalance);
            }
            Q_EMIT message(tr("Send Coins"), QString::fromStdString(util::ErrorString(res).translated),
                CClientUIInterface::MSG_ERROR);
            return TransactionCreationFailed;
        }

        // Reject absurdly high fee. (This can never happen because the
        // wallet never creates transactions with fee greater than
        // m_default_max_tx_fee. This merely a belt-and-suspenders check).
        CAmount normalizedFee = amountType == CASH ? nFeeRequired : m_wallet->estimateConvertedAmount(nFeeRequired, BOND);
        if (normalizedFee > DescaleAmount(m_wallet->getDefaultMaxTxFee(), getBestScaleFactor())) {
            return AbsurdFee;
        }
    }

    return SendCoinsReturn(OK);
}

void WalletModel::sendCoins(WalletModelTransaction& transaction)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        std::vector<std::pair<std::string, std::string>> vOrderForm;
        for (const SendCoinsRecipient &rcp : transaction.getRecipients())
        {
            if (!rcp.message.isEmpty()) // Message from normal peerfed:URI (peerfed:123...?message=example)
                vOrderForm.emplace_back("Message", rcp.message.toStdString());
        }

        auto& newTx = transaction.getWtx();
        wallet().commitTransaction(newTx, {} /* mapValue */, std::move(vOrderForm));

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *newTx;
        transaction_array.append((const char*)ssTx.data(), ssTx.size());
    }

    // Add addresses / update labels that we've sent to the address book,
    // and emit coinsSent signal for each recipient
    for (const SendCoinsRecipient &rcp : transaction.getRecipients())
    {
        {
            std::string strAddress = rcp.address.toStdString();
            CTxDestination dest = DecodeDestination(strAddress);
            std::string strLabel = rcp.label.toStdString();
            {
                // Check if we have a new address or an updated label
                std::string name;
                if (!m_wallet->getAddress(
                     dest, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr))
                {
                    m_wallet->setAddressBook(dest, strLabel, "send");
                }
                else if (name != strLabel)
                {
                    m_wallet->setAddressBook(dest, strLabel, ""); // "" means don't change purpose
                }
            }
        }
        Q_EMIT coinsSent(this, rcp, transaction_array);
    }

    checkBalanceChanged(m_wallet->getBalances()); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits
}

WalletModel::ConvertCoinsReturn WalletModel::prepareTransaction(WalletModelConversionTransaction &transaction, const CCoinControl& coinControl)
{
    // If no coin was manually selected, use the cached balance
    // Future: can merge this call with 'createConversionTransaction'.
    CAmount inputBalance = getAvailableBalance(transaction.getInputType(), &coinControl);

    if(transaction.getMaxInput() <= 0)
    {
        return InvalidInputAmount;
    }

    if(transaction.getMinOutput() < 0)
    {
        return InvalidOutputAmount;
    }

    if(transaction.getMaxInput() > inputBalance)
    {
        return InputAmountExceedsBalance;
    }

    {
        CAmount nFeeRequired = 0;
        CAmountType nFeeTypeRequired = 0;
        int nChangePosRet = -1;

        auto& newTx = transaction.getWtx();
        WalletConversionTxDetails txDetails = {transaction.getMaxInput(), transaction.getMinOutput(), transaction.getInputType(), transaction.getOutputType(), transaction.getRemainderType(), transaction.subtractFeeFromInput()};
        // Do not sign if private keys are disabled or if we are subtracting fees from input (since we will need to re-sign the transaction later after updating the output)
        bool sign = !wallet().privateKeysDisabled() && !transaction.subtractFeeFromInput();
        const auto& res = m_wallet->createConversionTransaction(txDetails, coinControl, sign, nChangePosRet, nFeeRequired, nFeeTypeRequired);
        newTx = res ? *res : nullptr;
        transaction.setTransactionFee(nFeeRequired, nFeeTypeRequired);

        if(!newTx)
        {
            if (nFeeTypeRequired == transaction.getOutputType() && nFeeRequired > transaction.getMinOutput())
            {
                return ConvertCoinsReturn(FeeExceedsOutputAmount);
            }
            if (nFeeTypeRequired == transaction.getInputType() && (nFeeRequired + transaction.getMaxInput()) > inputBalance)
            {
                return ConvertCoinsReturn(InputAmountWithFeeExceedsBalance);
            }
            Q_EMIT message(tr("Convert Coins"), QString::fromStdString(util::ErrorString(res).translated),
                CClientUIInterface::MSG_ERROR);
            return ConversionCreationFailed;
        }
    }

    return ConvertCoinsReturn(ConversionOK);
}

void WalletModel::convertCoins(WalletModelConversionTransaction& transaction)
{
    QByteArray transaction_array; /* store serialized transaction */

    {
        auto& newTx = transaction.getWtx();
        wallet().commitTransaction(newTx, {} /* mapValue */, {} /* orderForm */);

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *newTx;
        transaction_array.append((const char*)ssTx.data(), ssTx.size());
    }

    checkBalanceChanged(m_wallet->getBalances()); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits
}

bool WalletModel::signConversion(CMutableTransaction& mtx)
{
    if (!m_wallet->signBumpTransaction(mtx)) {
        QMessageBox::critical(nullptr, tr("Conversion error"), tr("Can't sign transaction."));
        return false;
    }
    return true;
}

OptionsModel* WalletModel::getOptionsModel() const
{
    return optionsModel;
}

AddressTableModel* WalletModel::getAddressTableModel() const
{
    return addressTableModel;
}

TransactionTableModel* WalletModel::getTransactionTableModel() const
{
    return transactionTableModel;
}

RecentRequestsTableModel* WalletModel::getRecentRequestsTableModel() const
{
    return recentRequestsTableModel;
}

WalletModel::EncryptionStatus WalletModel::getEncryptionStatus() const
{
    if(!m_wallet->isCrypted())
    {
        // A previous bug allowed for watchonly wallets to be encrypted (encryption keys set, but nothing is actually encrypted).
        // To avoid misrepresenting the encryption status of such wallets, we only return NoKeys for watchonly wallets that are unencrypted.
        if (m_wallet->privateKeysDisabled()) {
            return NoKeys;
        }
        return Unencrypted;
    }
    else if(m_wallet->isLocked())
    {
        return Locked;
    }
    else
    {
        return Unlocked;
    }
}

bool WalletModel::setWalletEncrypted(const SecureString& passphrase)
{
    return m_wallet->encryptWallet(passphrase);
}

bool WalletModel::setWalletLocked(bool locked, const SecureString &passPhrase)
{
    if(locked)
    {
        // Lock
        return m_wallet->lock();
    }
    else
    {
        // Unlock
        return m_wallet->unlock(passPhrase);
    }
}

bool WalletModel::changePassphrase(const SecureString &oldPass, const SecureString &newPass)
{
    m_wallet->lock(); // Make sure wallet is locked before attempting pass change
    return m_wallet->changeWalletPassphrase(oldPass, newPass);
}

// Handlers for core signals
static void NotifyUnload(WalletModel* walletModel)
{
    qDebug() << "NotifyUnload";
    bool invoked = QMetaObject::invokeMethod(walletModel, "unload");
    assert(invoked);
}

static void NotifyKeyStoreStatusChanged(WalletModel *walletmodel)
{
    qDebug() << "NotifyKeyStoreStatusChanged";
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateStatus", Qt::QueuedConnection);
    assert(invoked);
}

static void NotifyAddressBookChanged(WalletModel *walletmodel,
        const CTxDestination &address, const std::string &label, bool isMine,
        const std::string &purpose, ChangeType status)
{
    QString strAddress = QString::fromStdString(EncodeDestination(address));
    QString strLabel = QString::fromStdString(label);
    QString strPurpose = QString::fromStdString(purpose);

    qDebug() << "NotifyAddressBookChanged: " + strAddress + " " + strLabel + " isMine=" + QString::number(isMine) + " purpose=" + strPurpose + " status=" + QString::number(status);
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateAddressBook",
                              Q_ARG(QString, strAddress),
                              Q_ARG(QString, strLabel),
                              Q_ARG(bool, isMine),
                              Q_ARG(QString, strPurpose),
                              Q_ARG(int, status));
    assert(invoked);
}

static void NotifyTransactionChanged(WalletModel *walletmodel, const uint256 &hash, ChangeType status)
{
    Q_UNUSED(hash);
    Q_UNUSED(status);
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateTransaction", Qt::QueuedConnection);
    assert(invoked);
}

static void ShowProgress(WalletModel *walletmodel, const std::string &title, int nProgress)
{
    // emits signal "showProgress"
    bool invoked = QMetaObject::invokeMethod(walletmodel, "showProgress", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(title)),
                              Q_ARG(int, nProgress));
    assert(invoked);
}

static void NotifyWatchonlyChanged(WalletModel *walletmodel, bool fHaveWatchonly)
{
    bool invoked = QMetaObject::invokeMethod(walletmodel, "updateWatchOnlyFlag", Qt::QueuedConnection,
                              Q_ARG(bool, fHaveWatchonly));
    assert(invoked);
}

static void NotifyCanGetAddressesChanged(WalletModel* walletmodel)
{
    bool invoked = QMetaObject::invokeMethod(walletmodel, "canGetAddressesChanged");
    assert(invoked);
}

void WalletModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    m_handler_unload = m_wallet->handleUnload(std::bind(&NotifyUnload, this));
    m_handler_status_changed = m_wallet->handleStatusChanged(std::bind(&NotifyKeyStoreStatusChanged, this));
    m_handler_address_book_changed = m_wallet->handleAddressBookChanged(std::bind(NotifyAddressBookChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
    m_handler_transaction_changed = m_wallet->handleTransactionChanged(std::bind(NotifyTransactionChanged, this, std::placeholders::_1, std::placeholders::_2));
    m_handler_show_progress = m_wallet->handleShowProgress(std::bind(ShowProgress, this, std::placeholders::_1, std::placeholders::_2));
    m_handler_watch_only_changed = m_wallet->handleWatchOnlyChanged(std::bind(NotifyWatchonlyChanged, this, std::placeholders::_1));
    m_handler_can_get_addrs_changed = m_wallet->handleCanGetAddressesChanged(std::bind(NotifyCanGetAddressesChanged, this));
}

void WalletModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    m_handler_unload->disconnect();
    m_handler_status_changed->disconnect();
    m_handler_address_book_changed->disconnect();
    m_handler_transaction_changed->disconnect();
    m_handler_show_progress->disconnect();
    m_handler_watch_only_changed->disconnect();
    m_handler_can_get_addrs_changed->disconnect();
}

// WalletModel::UnlockContext implementation
WalletModel::UnlockContext WalletModel::requestUnlock()
{
    bool was_locked = getEncryptionStatus() == Locked;
    if(was_locked)
    {
        // Request UI to unlock wallet
        Q_EMIT requireUnlock();
    }
    // If wallet is still locked, unlock was failed or cancelled, mark context as invalid
    bool valid = getEncryptionStatus() != Locked;

    return UnlockContext(this, valid, was_locked);
}

WalletModel::UnlockContext::UnlockContext(WalletModel *_wallet, bool _valid, bool _relock):
        wallet(_wallet),
        valid(_valid),
        relock(_relock)
{
}

WalletModel::UnlockContext::~UnlockContext()
{
    if(valid && relock)
    {
        wallet->setWalletLocked(true);
    }
}

void WalletModel::UnlockContext::CopyFrom(UnlockContext&& rhs)
{
    // Transfer context; old object no longer relocks wallet
    *this = rhs;
    rhs.relock = false;
}

bool WalletModel::bumpFee(uint256 hash, uint256& new_hash)
{
    CCoinControl coin_control;
    coin_control.m_signal_bip125_rbf = true;
    std::vector<bilingual_str> errors;
    CAmounts old_fees;
    CAmounts new_fees;
    CMutableTransaction mtx;
    if (!m_wallet->createBumpTransaction(hash, coin_control, errors, old_fees, new_fees, mtx)) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Increasing transaction fee failed") + "<br />(" +
            (errors.size() ? QString::fromStdString(errors[0].translated) : "") +")");
        return false;
    }

    // Apply scale factor to displayed fee amounts
    if (getOptionsModel()->getShowScaledAmount(CASH)) {
        old_fees[CASH] = ScaleAmount(old_fees[CASH], getBestScaleFactor());
        new_fees[CASH] = ScaleAmount(new_fees[CASH], getBestScaleFactor());
    }
    if (getOptionsModel()->getShowScaledAmount(BOND)) {
        old_fees[BOND] = ScaleAmount(old_fees[BOND], getBestScaleFactor());
        new_fees[BOND] = ScaleAmount(new_fees[BOND], getBestScaleFactor());
    }

    // allow a user based fee verification
    /*: Asks a user if they would like to manually increase the fee of a transaction that has already been created. */
    QString questionString = tr("Do you want to increase the fee?");
    questionString.append("<br />");
    questionString.append("<table style=\"text-align: left;\">");
    questionString.append("<tr><td>");
    questionString.append(tr("Current fee:"));
    questionString.append("</td><td>");
    if (old_fees[CASH] > 0) {
        questionString.append(BitcoinUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(CASH), old_fees[CASH]));
        questionString.append("</td></tr><tr><td>");
    }
    if (old_fees[BOND] > 0) {
        questionString.append(BitcoinUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(BOND), old_fees[BOND]));
        questionString.append("</td></tr><tr><td>");
    }
    questionString.append(tr("Increase:"));
    questionString.append("</td><td>");
    if (old_fees[CASH] > 0) {
        questionString.append(BitcoinUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(CASH), new_fees[CASH] - old_fees[CASH]));
        questionString.append("</td></tr><tr><td>");
    }
    if (old_fees[BOND] > 0) {
        questionString.append(BitcoinUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(BOND), new_fees[BOND] - old_fees[BOND]));
        questionString.append("</td></tr><tr><td>");
    }
    questionString.append(tr("New fee:"));
    questionString.append("</td><td>");
    if (old_fees[CASH] > 0) {
        questionString.append(BitcoinUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(CASH), new_fees[CASH]));
        questionString.append("</td></tr>");
    }
    if (old_fees[CASH] > 0 && old_fees[BOND] > 0) {
        questionString.append("<td>");
    }
    if (old_fees[BOND] > 0) {
        questionString.append(BitcoinUnits::formatHtmlWithUnit(getOptionsModel()->getDisplayUnit(BOND), new_fees[BOND]));
        questionString.append("</td></tr>");
    }
    questionString.append("</table>");

    // Display warning in the "Confirm fee bump" window if the "Coin Control Features" option is enabled
    if (getOptionsModel()->getCoinControlFeatures()) {
        questionString.append("<br><br>");
        questionString.append(tr("Warning: This may pay the additional fee by reducing change outputs or adding inputs, when necessary. It may add a new change output if one does not already exist. These changes may potentially leak privacy."));
    }

    auto confirmationDialog = new SendConfirmationDialog(tr("Confirm fee bump"), questionString, "", "", SEND_CONFIRM_DELAY, !m_wallet->privateKeysDisabled(), getOptionsModel()->getEnablePSBTControls(), nullptr);
    confirmationDialog->setAttribute(Qt::WA_DeleteOnClose);
    // TODO: Replace QDialog::exec() with safer QDialog::show().
    const auto retval = static_cast<QMessageBox::StandardButton>(confirmationDialog->exec());

    // cancel sign&broadcast if user doesn't want to bump the fee
    if (retval != QMessageBox::Yes && retval != QMessageBox::Save) {
        return false;
    }

    WalletModel::UnlockContext ctx(requestUnlock());
    if(!ctx.isValid())
    {
        return false;
    }

    // Short-circuit if we are returning a bumped transaction PSBT to clipboard
    if (retval == QMessageBox::Save) {
        PartiallySignedTransaction psbtx(mtx);
        bool complete = false;
        const TransactionError err = wallet().fillPSBT(SIGHASH_ALL, false /* sign */, true /* bip32derivs */, nullptr, psbtx, complete);
        if (err != TransactionError::OK || complete) {
            QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Can't draft transaction."));
            return false;
        }
        // Serialize the PSBT
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << psbtx;
        GUIUtil::setClipboard(EncodeBase64(ssTx.str()).c_str());
        Q_EMIT message(tr("PSBT copied"), "Copied to clipboard", CClientUIInterface::MSG_INFORMATION);
        return true;
    }

    assert(!m_wallet->privateKeysDisabled());

    // sign bumped transaction
    if (!m_wallet->signBumpTransaction(mtx)) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Can't sign transaction."));
        return false;
    }
    // commit the bumped transaction
    if(!m_wallet->commitBumpTransaction(hash, std::move(mtx), errors, new_hash)) {
        QMessageBox::critical(nullptr, tr("Fee bump error"), tr("Could not commit transaction") + "<br />(" +
            QString::fromStdString(errors[0].translated)+")");
        return false;
    }
    return true;
}

bool WalletModel::displayAddress(std::string sAddress) const
{
    CTxDestination dest = DecodeDestination(sAddress);
    bool res = false;
    try {
        res = m_wallet->displayAddress(dest);
    } catch (const std::runtime_error& e) {
        QMessageBox::critical(nullptr, tr("Can't display address"), e.what());
    }
    return res;
}

bool WalletModel::isWalletEnabled()
{
   return !gArgs.GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET);
}

QString WalletModel::getWalletName() const
{
    return QString::fromStdString(m_wallet->getWalletName());
}

QString WalletModel::getDisplayName() const
{
    const QString name = getWalletName();
    return name.isEmpty() ? "["+tr("default wallet")+"]" : name;
}

bool WalletModel::isMultiwallet() const
{
    return m_node.walletLoader().getWallets().size() > 1;
}

void WalletModel::refresh(bool pk_hash_only)
{
    addressTableModel = new AddressTableModel(this, pk_hash_only);
}

uint256 WalletModel::getLastBlockProcessed() const
{
    return m_client_model ? m_client_model->getBestBlockHash() : uint256{};
}

CAmountScaleFactor WalletModel::getBestScaleFactor() const
{
    return m_client_model ? m_client_model->getBestScaleFactor() : BASE_FACTOR;
}

int64_t WalletModel::getBestInterestRate() const
{
    return m_client_model ? m_client_model->getBestInterestRate() : 0;
}

CAmount WalletModel::getAvailableBalance(CAmountType type, const CCoinControl* control)
{
    return control && control->HasSelected() ? wallet().getAvailableBalance(type, *control) : m_cached_balances.forType(type).balance;
}
