// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/wallet.h>

#include <qt/convertcoinsdialog.h>
#include <qt/forms/ui_convertcoinsdialog.h>

#include <qt/addresstablemodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/receiverequestdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/walletmodel.h>

#include <QAction>
#include <QCursor>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QTextDocument>

ConvertCoinsDialog::ConvertCoinsDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::ConvertCoinsDialog),
    model(nullptr),
    platformStyle(_platformStyle)
{
    ui->setupUi(this);

    if (!_platformStyle->getImagesOnButtons()) {
        ui->clearButton->setIcon(QIcon());
        ui->receiveButton->setIcon(QIcon());
        ui->showRequestButton->setIcon(QIcon());
        ui->removeRequestButton->setIcon(QIcon());
    } else {
        ui->clearButton->setIcon(_platformStyle->SingleColorIcon(":/icons/remove"));
        ui->receiveButton->setIcon(_platformStyle->SingleColorIcon(":/icons/receiving_addresses"));
        ui->showRequestButton->setIcon(_platformStyle->SingleColorIcon(":/icons/eye"));
        ui->removeRequestButton->setIcon(_platformStyle->SingleColorIcon(":/icons/remove"));
    }

    // context menu
    contextMenu = new QMenu(this);
    contextMenu->addAction(tr("Copy &URI"), this, &ConvertCoinsDialog::copyURI);
    contextMenu->addAction(tr("&Copy address"), this, &ConvertCoinsDialog::copyAddress);
    copyLabelAction = contextMenu->addAction(tr("Copy &label"), this, &ConvertCoinsDialog::copyLabel);
    copyMessageAction = contextMenu->addAction(tr("Copy &message"), this, &ConvertCoinsDialog::copyMessage);
    copyAmountAction = contextMenu->addAction(tr("Copy &amount"), this, &ConvertCoinsDialog::copyAmount);
    connect(ui->recentRequestsView, &QWidget::customContextMenuRequested, this, &ConvertCoinsDialog::showMenu);

    connect(ui->clearButton, &QPushButton::clicked, this, &ConvertCoinsDialog::clear);

    QTableView* tableView = ui->recentRequestsView;
    tableView->verticalHeader()->hide();
    tableView->setAlternatingRowColors(true);
    tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView->setSelectionMode(QAbstractItemView::ContiguousSelection);

    QSettings settings;
    if (!tableView->horizontalHeader()->restoreState(settings.value("RecentRequestsViewHeaderState").toByteArray())) {
        tableView->setColumnWidth(RecentRequestsTableModel::Date, DATE_COLUMN_WIDTH);
        tableView->setColumnWidth(RecentRequestsTableModel::Label, LABEL_COLUMN_WIDTH);
        tableView->setColumnWidth(RecentRequestsTableModel::Amount, AMOUNT_MINIMUM_COLUMN_WIDTH);
        tableView->horizontalHeader()->setMinimumSectionSize(MINIMUM_COLUMN_WIDTH);
        tableView->horizontalHeader()->setStretchLastSection(true);
    }
}

void ConvertCoinsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        _model->getRecentRequestsTableModel()->sort(RecentRequestsTableModel::Date, Qt::DescendingOrder);
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &ConvertCoinsDialog::updateDisplayUnit);
        updateDisplayUnit();

        QTableView* tableView = ui->recentRequestsView;
        tableView->setModel(_model->getRecentRequestsTableModel());
        tableView->sortByColumn(RecentRequestsTableModel::Date, Qt::DescendingOrder);

        connect(tableView->selectionModel(),
            &QItemSelectionModel::selectionChanged, this,
            &ConvertCoinsDialog::recentRequestsView_selectionChanged);

        // Populate address type dropdown and select default
        auto add_address_type = [&](OutputType type, const QString& text, const QString& tooltip) {
            const auto index = ui->addressType->count();
            ui->addressType->addItem(text, (int) type);
            ui->addressType->setItemData(index, tooltip, Qt::ToolTipRole);
            if (model->wallet().getDefaultAddressType() == type) ui->addressType->setCurrentIndex(index);
        };
        add_address_type(OutputType::LEGACY, "Base58 (Legacy)", "Not recommended due to higher fees and less protection against typos.");
        add_address_type(OutputType::P2SH_SEGWIT, "Base58 (P2SH-SegWit)", "Generates an address compatible with older wallets.");
        add_address_type(OutputType::BECH32, "Bech32 (SegWit)", "Generates a native segwit address (BIP-173). Some old wallets don't support it.");
        if (model->wallet().taprootEnabled()) {
            add_address_type(OutputType::BECH32M, "Bech32m (Taproot)", "Bech32m (BIP-350) is an upgrade to Bech32, wallet support is still limited.");
        }

        // Set the button to be enabled or disabled based on whether the wallet can give out new addresses.
        ui->receiveButton->setEnabled(model->wallet().canGetAddresses());

        // Enable/disable the receive button if the wallet is now able/unable to give out new addresses.
        connect(model, &WalletModel::canGetAddressesChanged, [this] {
            ui->receiveButton->setEnabled(model->wallet().canGetAddresses());
        });
    }
}

ConvertCoinsDialog::~ConvertCoinsDialog()
{
    QSettings settings;
    settings.setValue("RecentRequestsViewHeaderState", ui->recentRequestsView->horizontalHeader()->saveState());
    delete ui;
}

void ConvertCoinsDialog::clear()
{
    ui->reqAmount->clear();
    ui->reqLabel->setText("");
    ui->reqMessage->setText("");
    updateDisplayUnit();
}

void ConvertCoinsDialog::reject()
{
    clear();
}

void ConvertCoinsDialog::accept()
{
    clear();
}

void ConvertCoinsDialog::updateDisplayUnit()
{
    if(model && model->getOptionsModel())
    {
        ui->reqAmount->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    }
}

void ConvertCoinsDialog::on_receiveButton_clicked()
{
    if(!model || !model->getOptionsModel() || !model->getAddressTableModel() || !model->getRecentRequestsTableModel())
        return;

    QString address;
    QString label = ui->reqLabel->text();
    /* Generate new receiving address */
    const OutputType address_type = (OutputType)ui->addressType->currentData().toInt();
    address = model->getAddressTableModel()->addRow(AddressTableModel::Receive, label, "", address_type);

    switch(model->getAddressTableModel()->getEditStatus())
    {
    case AddressTableModel::EditStatus::OK: {
        // Success
        SendCoinsRecipient info(address, label, CASH, // TODO: Implement amount type
            ui->reqAmount->value(), ui->reqMessage->text());
        ReceiveRequestDialog *dialog = new ReceiveRequestDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setModel(model);
        dialog->setInfo(info);
        dialog->show();

        /* Store request for later reference */
        model->getRecentRequestsTableModel()->addNewRequest(info);
        break;
    }
    case AddressTableModel::EditStatus::WALLET_UNLOCK_FAILURE:
        QMessageBox::critical(this, windowTitle(),
            tr("Could not unlock wallet."),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case AddressTableModel::EditStatus::KEY_GENERATION_FAILURE:
        QMessageBox::critical(this, windowTitle(),
            tr("Could not generate new %1 address").arg(QString::fromStdString(FormatOutputType(address_type))),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    // These aren't valid return values for our action
    case AddressTableModel::EditStatus::INVALID_ADDRESS:
    case AddressTableModel::EditStatus::DUPLICATE_ADDRESS:
    case AddressTableModel::EditStatus::NO_CHANGES:
        assert(false);
    }
    clear();
}

void ConvertCoinsDialog::on_recentRequestsView_doubleClicked(const QModelIndex &index)
{
    const RecentRequestsTableModel *submodel = model->getRecentRequestsTableModel();
    ReceiveRequestDialog *dialog = new ReceiveRequestDialog(this);
    dialog->setModel(model);
    dialog->setInfo(submodel->entry(index.row()).recipient);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void ConvertCoinsDialog::recentRequestsView_selectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    // Enable Show/Remove buttons only if anything is selected.
    bool enable = !ui->recentRequestsView->selectionModel()->selectedRows().isEmpty();
    ui->showRequestButton->setEnabled(enable);
    ui->removeRequestButton->setEnabled(enable);
}

void ConvertCoinsDialog::on_showRequestButton_clicked()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return;
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();

    for (const QModelIndex& index : selection) {
        on_recentRequestsView_doubleClicked(index);
    }
}

void ConvertCoinsDialog::on_removeRequestButton_clicked()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return;
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();
    if(selection.empty())
        return;
    // correct for selection mode ContiguousSelection
    QModelIndex firstIndex = selection.at(0);
    model->getRecentRequestsTableModel()->removeRows(firstIndex.row(), selection.length(), firstIndex.parent());
}

QModelIndex ConvertCoinsDialog::selectedRow()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return QModelIndex();
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();
    if(selection.empty())
        return QModelIndex();
    // correct for selection mode ContiguousSelection
    QModelIndex firstIndex = selection.at(0);
    return firstIndex;
}

// copy column of selected row to clipboard
void ConvertCoinsDialog::copyColumnToClipboard(int column)
{
    QModelIndex firstIndex = selectedRow();
    if (!firstIndex.isValid()) {
        return;
    }
    GUIUtil::setClipboard(model->getRecentRequestsTableModel()->index(firstIndex.row(), column).data(Qt::EditRole).toString());
}

// context menu
void ConvertCoinsDialog::showMenu(const QPoint &point)
{
    const QModelIndex sel = selectedRow();
    if (!sel.isValid()) {
        return;
    }

    // disable context menu actions when appropriate
    const RecentRequestsTableModel* const submodel = model->getRecentRequestsTableModel();
    const RecentRequestEntry& req = submodel->entry(sel.row());
    copyLabelAction->setDisabled(req.recipient.label.isEmpty());
    copyMessageAction->setDisabled(req.recipient.message.isEmpty());
    copyAmountAction->setDisabled(req.recipient.amount == 0);

    contextMenu->exec(QCursor::pos());
}

// context menu action: copy URI
void ConvertCoinsDialog::copyURI()
{
    QModelIndex sel = selectedRow();
    if (!sel.isValid()) {
        return;
    }

    const RecentRequestsTableModel * const submodel = model->getRecentRequestsTableModel();
    const QString uri = GUIUtil::formatBitcoinURI(submodel->entry(sel.row()).recipient);
    GUIUtil::setClipboard(uri);
}

// context menu action: copy address
void ConvertCoinsDialog::copyAddress()
{
    const QModelIndex sel = selectedRow();
    if (!sel.isValid()) {
        return;
    }

    const RecentRequestsTableModel* const submodel = model->getRecentRequestsTableModel();
    const QString address = submodel->entry(sel.row()).recipient.address;
    GUIUtil::setClipboard(address);
}

// context menu action: copy label
void ConvertCoinsDialog::copyLabel()
{
    copyColumnToClipboard(RecentRequestsTableModel::Label);
}

// context menu action: copy message
void ConvertCoinsDialog::copyMessage()
{
    copyColumnToClipboard(RecentRequestsTableModel::Message);
}

// context menu action: copy amount
void ConvertCoinsDialog::copyAmount()
{
    copyColumnToClipboard(RecentRequestsTableModel::Amount);
}
