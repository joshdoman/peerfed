// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/walletview.h>

#include <qt/addressbookpage.h>
#include <qt/askpassphrasedialog.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/overviewpage.h>
#include <qt/platformstyle.h>
#include <qt/convertcoinsdialog.h>
#include <qt/receivecoinsdialog.h>
#include <qt/sendcoinsdialog.h>
#include <qt/signverifymessagedialog.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/walletmodel.h>

#include <interfaces/node.h>
#include <node/interface_ui.h>
#include <util/strencodings.h>

#include <QAction>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QProgressDialog>
#include <QPushButton>
#include <QVBoxLayout>

WalletView::WalletView(WalletModel* wallet_model, const PlatformStyle* _platformStyle, QWidget* parent)
    : QStackedWidget(parent),
      clientModel(nullptr),
      walletModel(wallet_model),
      platformStyle(_platformStyle)
{
    assert(walletModel);

    // Create tabs
    overviewPage = new OverviewPage(platformStyle);
    overviewPage->setWalletModel(walletModel);

    transactionsPage = new QWidget(this);
    QVBoxLayout *vbox = new QVBoxLayout();
    QHBoxLayout *hbox_buttons = new QHBoxLayout();
    transactionView = new TransactionView(platformStyle, this);
    transactionView->setModel(walletModel);

    vbox->addWidget(transactionView);
    QPushButton *exportButton = new QPushButton(tr("&Export"), this);
    exportButton->setToolTip(tr("Export the data in the current tab to a file"));
    if (platformStyle->getImagesOnButtons()) {
        exportButton->setIcon(platformStyle->SingleColorIcon(":/icons/export"));
    }
    hbox_buttons->addStretch();
    hbox_buttons->addWidget(exportButton);
    vbox->addLayout(hbox_buttons);
    transactionsPage->setLayout(vbox);

    convertCoinsPage = new ConvertCoinsDialog(platformStyle);
    convertCoinsPage->setModel(walletModel);

    receiveCoinsPage = new ReceiveCoinsDialog(platformStyle);
    receiveCoinsPage->setModel(walletModel);

    sendCoinsPage = new SendCoinsDialog(platformStyle);
    sendCoinsPage->setModel(walletModel);

    usedSendingAddressesPage = new AddressBookPage(platformStyle, AddressBookPage::ForEditing, AddressBookPage::SendingTab, this);
    usedSendingAddressesPage->setModel(walletModel->getAddressTableModel());

    usedReceivingAddressesPage = new AddressBookPage(platformStyle, AddressBookPage::ForEditing, AddressBookPage::ReceivingTab, this);
    usedReceivingAddressesPage->setModel(walletModel->getAddressTableModel());

    addWidget(overviewPage);
    addWidget(transactionsPage);
    addWidget(convertCoinsPage);
    addWidget(receiveCoinsPage);
    addWidget(sendCoinsPage);

    connect(overviewPage, &OverviewPage::transactionClicked, this, &WalletView::transactionClicked);
    // Clicking on a transaction on the overview pre-selects the transaction on the transaction history page
    connect(overviewPage, &OverviewPage::transactionClicked, transactionView, qOverload<const QModelIndex&>(&TransactionView::focusTransaction));

    connect(overviewPage, &OverviewPage::outOfSyncWarningClicked, this, &WalletView::outOfSyncWarningClicked);

    connect(sendCoinsPage, &SendCoinsDialog::coinsSent, this, &WalletView::coinsSent);
    // Highlight transaction after send
    connect(sendCoinsPage, &SendCoinsDialog::coinsSent, transactionView, qOverload<const uint256&>(&TransactionView::focusTransaction));

    connect(convertCoinsPage, &ConvertCoinsDialog::coinsConverted, this, &WalletView::coinsConverted);
    // Highlight transaction after conversion
    connect(convertCoinsPage, &ConvertCoinsDialog::coinsConverted, transactionView, qOverload<const uint256&>(&TransactionView::focusTransaction));

    // Clicking on "Export" allows to export the transaction list
    connect(exportButton, &QPushButton::clicked, transactionView, &TransactionView::exportClicked);

    // Pass through messages from sendCoinsPage
    connect(sendCoinsPage, &SendCoinsDialog::message, this, &WalletView::message);
    // Pass through messages from convertCoinsPage
    connect(convertCoinsPage, &ConvertCoinsDialog::message, this, &WalletView::message);
    // Pass through messages from transactionView
    connect(transactionView, &TransactionView::message, this, &WalletView::message);

    connect(this, &WalletView::setPrivacy, overviewPage, &OverviewPage::setPrivacy);

    // Receive and pass through messages from wallet model
    connect(walletModel, &WalletModel::message, this, &WalletView::message);

    // Handle changes in encryption status
    connect(walletModel, &WalletModel::encryptionStatusChanged, this, &WalletView::encryptionStatusChanged);

    // Balloon pop-up for new transaction
    connect(walletModel->getTransactionTableModel(), &TransactionTableModel::rowsInserted, this, &WalletView::processNewTransaction);

    // Ask for passphrase if needed
    connect(walletModel, &WalletModel::requireUnlock, this, &WalletView::unlockWallet);

    // Show progress dialog
    connect(walletModel, &WalletModel::showProgress, this, &WalletView::showProgress);
}

WalletView::~WalletView() = default;

void WalletView::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    overviewPage->setClientModel(_clientModel);
    sendCoinsPage->setClientModel(_clientModel);
    convertCoinsPage->setClientModel(_clientModel);
    walletModel->setClientModel(_clientModel);
}

void WalletView::processNewTransaction(const QModelIndex& parent, int start, int /*end*/)
{
    // Prevent balloon-spam when initial block download is in progress
    if (!clientModel || clientModel->node().isInitialBlockDownload()) {
        return;
    }

    TransactionTableModel *ttm = walletModel->getTransactionTableModel();
    if (!ttm || ttm->processingQueuedTransactions())
        return;

    QString date = ttm->index(start, TransactionTableModel::Date, parent).data().toString();
    qint64 cashAmount = ttm->index(start, TransactionTableModel::CashAmount, parent).data(Qt::EditRole).toULongLong();
    qint64 bondAmount = ttm->index(start, TransactionTableModel::BondAmount, parent).data(Qt::EditRole).toULongLong();
    QString type = ttm->index(start, TransactionTableModel::Type, parent).data().toString();
    QModelIndex index = ttm->index(start, 0, parent);
    QString address = ttm->data(index, TransactionTableModel::AddressRole).toString();
    QString label = GUIUtil::HtmlEscape(ttm->data(index, TransactionTableModel::LabelRole).toString());

    CAmountType amountType = cashAmount != 0 ? CASH : BOND;
    BitcoinUnit unit = walletModel->getOptionsModel()->getDisplayUnit(amountType);
    Q_EMIT incomingTransaction(date, unit, cashAmount + bondAmount, type, address, label, GUIUtil::HtmlEscape(walletModel->getWalletName()));

    // Check if this is a "Mined" transaction
    TransactionRecord::Type recordType = (TransactionRecord::Type)ttm->data(index, TransactionTableModel::TypeRole).toInt();
    if (recordType == TransactionRecord::Type::Generated) {
        // Emit a second notification for the second mined output
        // TODO: Combine into a single notification
        int next = start + 1;
        QModelIndex index = ttm->index(next, 0, parent);
        if (!index.isValid())
            return;
        QString date = ttm->index(next, TransactionTableModel::Date, parent).data().toString();
        qint64 cashAmount = ttm->index(next, TransactionTableModel::CashAmount, parent).data(Qt::EditRole).toULongLong();
        qint64 bondAmount = ttm->index(next, TransactionTableModel::BondAmount, parent).data(Qt::EditRole).toULongLong();
        QString type = ttm->index(next, TransactionTableModel::Type, parent).data().toString();
        QString address = ttm->data(index, TransactionTableModel::AddressRole).toString();
        QString label = GUIUtil::HtmlEscape(ttm->data(index, TransactionTableModel::LabelRole).toString());

        CAmountType amountType = cashAmount != 0 ? CASH : BOND;
        BitcoinUnit unit = walletModel->getOptionsModel()->getDisplayUnit(amountType);
        Q_EMIT incomingTransaction(date, unit, cashAmount + bondAmount, type, address, label, GUIUtil::HtmlEscape(walletModel->getWalletName()));
    }
}

void WalletView::gotoOverviewPage()
{
    setCurrentWidget(overviewPage);
}

void WalletView::gotoHistoryPage()
{
    setCurrentWidget(transactionsPage);
}

void WalletView::gotoConvertCoinsPage()
{
    setCurrentWidget(convertCoinsPage);
}

void WalletView::gotoReceiveCoinsPage()
{
    setCurrentWidget(receiveCoinsPage);
}

void WalletView::gotoSendCoinsPage(QString addr)
{
    setCurrentWidget(sendCoinsPage);

    if (!addr.isEmpty())
        sendCoinsPage->setAddress(addr);
}

void WalletView::gotoSignMessageTab(QString addr)
{
    // calls show() in showTab_SM()
    SignVerifyMessageDialog *signVerifyMessageDialog = new SignVerifyMessageDialog(platformStyle, this);
    signVerifyMessageDialog->setAttribute(Qt::WA_DeleteOnClose);
    signVerifyMessageDialog->setModel(walletModel);
    signVerifyMessageDialog->showTab_SM(true);

    if (!addr.isEmpty())
        signVerifyMessageDialog->setAddress_SM(addr);
}

void WalletView::gotoVerifyMessageTab(QString addr)
{
    // calls show() in showTab_VM()
    SignVerifyMessageDialog *signVerifyMessageDialog = new SignVerifyMessageDialog(platformStyle, this);
    signVerifyMessageDialog->setAttribute(Qt::WA_DeleteOnClose);
    signVerifyMessageDialog->setModel(walletModel);
    signVerifyMessageDialog->showTab_VM(true);

    if (!addr.isEmpty())
        signVerifyMessageDialog->setAddress_VM(addr);
}

bool WalletView::handlePaymentRequest(const SendCoinsRecipient& recipient)
{
    return sendCoinsPage->handlePaymentRequest(recipient);
}

void WalletView::showOutOfSyncWarning(bool fShow)
{
    overviewPage->showOutOfSyncWarning(fShow);
}

void WalletView::encryptWallet()
{
    auto dlg = new AskPassphraseDialog(AskPassphraseDialog::Encrypt, this);
    dlg->setModel(walletModel);
    connect(dlg, &QDialog::finished, this, &WalletView::encryptionStatusChanged);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void WalletView::backupWallet()
{
    QString filename = GUIUtil::getSaveFileName(this,
        tr("Backup Wallet"), QString(),
        //: Name of the wallet data file format.
        tr("Wallet Data") + QLatin1String(" (*.dat)"), nullptr);

    if (filename.isEmpty())
        return;

    if (!walletModel->wallet().backupWallet(filename.toLocal8Bit().data())) {
        Q_EMIT message(tr("Backup Failed"), tr("There was an error trying to save the wallet data to %1.").arg(filename),
            CClientUIInterface::MSG_ERROR);
        }
    else {
        Q_EMIT message(tr("Backup Successful"), tr("The wallet data was successfully saved to %1.").arg(filename),
            CClientUIInterface::MSG_INFORMATION);
    }
}

void WalletView::changePassphrase()
{
    auto dlg = new AskPassphraseDialog(AskPassphraseDialog::ChangePass, this);
    dlg->setModel(walletModel);
    GUIUtil::ShowModalDialogAsynchronously(dlg);
}

void WalletView::unlockWallet()
{
    // Unlock wallet when requested by wallet model
    if (walletModel->getEncryptionStatus() == WalletModel::Locked) {
        AskPassphraseDialog dlg(AskPassphraseDialog::Unlock, this);
        dlg.setModel(walletModel);
        // A modal dialog must be synchronous here as expected
        // in the WalletModel::requestUnlock() function.
        dlg.exec();
    }
}

void WalletView::usedSendingAddresses()
{
    GUIUtil::bringToFront(usedSendingAddressesPage);
}

void WalletView::usedReceivingAddresses()
{
    GUIUtil::bringToFront(usedReceivingAddressesPage);
}

void WalletView::showProgress(const QString &title, int nProgress)
{
    if (nProgress == 0) {
        progressDialog = new QProgressDialog(title, tr("Cancel"), 0, 100);
        GUIUtil::PolishProgressDialog(progressDialog);
        progressDialog->setWindowModality(Qt::ApplicationModal);
        progressDialog->setAutoClose(false);
        progressDialog->setValue(0);
    } else if (nProgress == 100) {
        if (progressDialog) {
            progressDialog->close();
            progressDialog->deleteLater();
            progressDialog = nullptr;
        }
    } else if (progressDialog) {
        if (progressDialog->wasCanceled()) {
            getWalletModel()->wallet().abortRescan();
        } else {
            progressDialog->setValue(nProgress);
        }
    }
}
