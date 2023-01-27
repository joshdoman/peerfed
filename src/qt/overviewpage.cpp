// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/overviewpage.h>
#include <qt/forms/ui_overviewpage.h>

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionoverviewwidget.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QDateTime>
#include <QPainter>
#include <QStatusTipEvent>

#include <algorithm>
#include <map>

#define DECORATION_SIZE 54
#define NUM_ITEMS 5

Q_DECLARE_METATYPE(interfaces::WalletBalances)

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(const PlatformStyle* _platformStyle, QObject* parent = nullptr)
        : QAbstractItemDelegate(parent), platformStyle(_platformStyle)
    {
        connect(this, &TxViewDelegate::width_changed, this, &TxViewDelegate::sizeHintChanged);
    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const override
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(TransactionTableModel::RawDecorationRole));
        QRect mainRect = option.rect;
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int rectHeight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace, rectHeight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+rectHeight, mainRect.width() - xspace, rectHeight);
        icon = platformStyle->SingleColorIcon(icon);
        icon.paint(painter, decorationRect);

        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        CAmountType amountType = index.data(TransactionTableModel::AmountTypeRole).toLongLong();
        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(value.canConvert<QBrush>())
        {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        if (index.data(TransactionTableModel::WatchonlyRole).toBool()) {
            QIcon iconWatchonly = qvariant_cast<QIcon>(index.data(TransactionTableModel::WatchonlyDecorationRole));
            QRect watchonlyRect(addressRect.left(), addressRect.top(), 16, addressRect.height());
            iconWatchonly = platformStyle->TextColorIcon(iconWatchonly);
            iconWatchonly.paint(painter, watchonlyRect);
            addressRect.setLeft(addressRect.left() + watchonlyRect.width() + 5);
        }

        painter->setPen(foreground);
        QRect boundingRect;
        painter->drawText(addressRect, Qt::AlignLeft | Qt::AlignVCenter, address, &boundingRect);

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);

        BitcoinUnit unit = (amountType == CASH) ? cashUnit : bondUnit;
        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true, BitcoinUnits::SeparatorStyle::ALWAYS);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }

        QRect amount_bounding_rect;
        painter->drawText(amountRect, Qt::AlignRight | Qt::AlignVCenter, amountText, &amount_bounding_rect);

        painter->setPen(option.palette.color(QPalette::Text));
        QRect date_bounding_rect;
        painter->drawText(amountRect, Qt::AlignLeft | Qt::AlignVCenter, GUIUtil::dateTimeStr(date), &date_bounding_rect);

        // 0.4*date_bounding_rect.width() is used to visually distinguish a date from an amount.
        const int minimum_width = 1.4 * date_bounding_rect.width() + amount_bounding_rect.width();
        const auto search = m_minimum_width.find(index.row());
        if (search == m_minimum_width.end() || search->second != minimum_width) {
            m_minimum_width[index.row()] = minimum_width;
            Q_EMIT width_changed(index);
        }

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto search = m_minimum_width.find(index.row());
        const int minimum_text_width = search == m_minimum_width.end() ? 0 : search->second;
        return {DECORATION_SIZE + 8 + minimum_text_width, DECORATION_SIZE};
    }

    BitcoinUnit cashUnit{BitcoinUnit::CASH};
    BitcoinUnit bondUnit{BitcoinUnit::BOND};

Q_SIGNALS:
    //! An intermediate signal for emitting from the `paint() const` member function.
    void width_changed(const QModelIndex& index) const;

private:
    const PlatformStyle* platformStyle;
    mutable std::map<int, int> m_minimum_width;
};

#include <qt/overviewpage.moc>

OverviewPage::OverviewPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    clientModel(nullptr),
    walletModel(nullptr),
    m_platform_style{platformStyle},
    txdelegate(new TxViewDelegate(platformStyle, this))
{
    ui->setupUi(this);

    // use a SingleColorIcon for the "out of sync warning" icon
    QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
    ui->labelTransactionsStatus->setIcon(icon);
    ui->labelWalletStatus0->setIcon(icon);
    ui->labelWalletStatus1->setIcon(icon);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, &TransactionOverviewWidget::clicked, this, &OverviewPage::handleTransactionClicked);

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
    connect(ui->labelWalletStatus0, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
    connect(ui->labelWalletStatus1, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
    connect(ui->labelTransactionsStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    const auto& balances = walletModel->getCachedBalance();
    if (balances.cash.balance != -1 && balances.bond.balance != -1) {
        setBalance(balances);
    }

    ui->listTransactions->setVisible(!m_privacy);

    const QString status_tip = m_privacy ? tr("Privacy mode activated for the Overview tab. To unmask the values, uncheck Settings->Mask values.") : "";
    setStatusTip(status_tip);
    QStatusTipEvent event(status_tip);
    QApplication::sendEvent(this, &event);
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(const interfaces::WalletBalances& balances)
{
    interfaces::WalletBalancesForAmountType cash = balances.cash;
    interfaces::WalletBalancesForAmountType bond = balances.bond;
    BitcoinUnit cashUnit = walletModel->getOptionsModel()->getDisplayUnit(CASH);
    BitcoinUnit bondUnit = walletModel->getOptionsModel()->getDisplayUnit(BOND);
    if (walletModel->wallet().isLegacy()) {
        if (walletModel->wallet().privateKeysDisabled()) {
            ui->labelBalance0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelUnconfirmed0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.unconfirmed_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelImmature0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelTotal0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.watch_only_balance + bond.unconfirmed_watch_only_balance + bond.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));

            ui->labelBalance1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelUnconfirmed1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.unconfirmed_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelImmature1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelTotal1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.watch_only_balance + cash.unconfirmed_watch_only_balance + cash.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        } else {
            ui->labelBalance0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelUnconfirmed0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.unconfirmed_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelImmature0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelTotal0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.balance + bond.unconfirmed_balance + bond.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchAvailable0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchPending0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.unconfirmed_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchImmature0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchTotal0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.watch_only_balance + bond.unconfirmed_watch_only_balance + bond.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));

            ui->labelBalance1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelUnconfirmed1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.unconfirmed_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelImmature1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelTotal1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.balance + cash.unconfirmed_balance + cash.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchAvailable1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchPending1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.unconfirmed_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchImmature1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
            ui->labelWatchTotal1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.watch_only_balance + cash.unconfirmed_watch_only_balance + cash.immature_watch_only_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        }
    } else {
        ui->labelBalance0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        ui->labelUnconfirmed0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.unconfirmed_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        ui->labelImmature0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        ui->labelTotal0->setText(BitcoinUnits::formatWithPrivacy(bondUnit, bond.balance + bond.unconfirmed_balance + bond.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));

        ui->labelBalance1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        ui->labelUnconfirmed1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.unconfirmed_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        ui->labelImmature1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
        ui->labelTotal1->setText(BitcoinUnits::formatWithPrivacy(cashUnit, cash.balance + cash.unconfirmed_balance + cash.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    }
    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature0 = bond.immature_balance != 0;
    bool showWatchOnlyImmature0 = bond.immature_watch_only_balance != 0;
    bool showImmature1 = cash.immature_balance != 0;
    bool showWatchOnlyImmature1 = cash.immature_watch_only_balance != 0;

    // for symmetry reasons also show immature label when the watch-only one is shown
    ui->labelImmature0->setVisible(showImmature0 || showWatchOnlyImmature0);
    ui->labelImmatureText0->setVisible(showImmature0 || showWatchOnlyImmature0);
    ui->labelWatchImmature0->setVisible(!walletModel->wallet().privateKeysDisabled() && showWatchOnlyImmature0); // show watch-only immature balance

    ui->labelImmature1->setVisible(showImmature1 || showWatchOnlyImmature1);
    ui->labelImmatureText1->setVisible(showImmature1 || showWatchOnlyImmature1);
    ui->labelWatchImmature1->setVisible(!walletModel->wallet().privateKeysDisabled() && showWatchOnlyImmature1); // show watch-only immature balance
}

// show/hide watch-only labels
void OverviewPage::updateWatchOnlyLabels(bool showWatchOnly)
{
    ui->labelSpendable0->setVisible(showWatchOnly);      // show spendable label (only when watch-only is active)
    ui->labelWatchonly0->setVisible(showWatchOnly);      // show watch-only label
    ui->lineWatchBalance0->setVisible(showWatchOnly);    // show watch-only balance separator line
    ui->labelWatchAvailable0->setVisible(showWatchOnly); // show watch-only available balance
    ui->labelWatchPending0->setVisible(showWatchOnly);   // show watch-only pending balance
    ui->labelWatchTotal0->setVisible(showWatchOnly);     // show watch-only total balance

    ui->labelSpendable1->setVisible(showWatchOnly);      // show spendable label (only when watch-only is active)
    ui->labelWatchonly1->setVisible(showWatchOnly);      // show watch-only label
    ui->lineWatchBalance1->setVisible(showWatchOnly);    // show watch-only balance separator line
    ui->labelWatchAvailable1->setVisible(showWatchOnly); // show watch-only available balance
    ui->labelWatchPending1->setVisible(showWatchOnly);   // show watch-only pending balance
    ui->labelWatchTotal1->setVisible(showWatchOnly);     // show watch-only total balance

    if (!showWatchOnly)
        ui->labelWatchImmature0->hide();
        ui->labelWatchImmature1->hide();
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if (model) {
        // Show warning, for example if this is a prerelease version
        connect(model, &ClientModel::alertsChanged, this, &OverviewPage::updateAlerts);
        updateAlerts(model->getStatusBarWarnings());

        connect(model->getOptionsModel(), &OptionsModel::useEmbeddedMonospacedFontChanged, this, &OverviewPage::setMonospacedFont);
        setMonospacedFont(model->getOptionsModel()->getUseEmbeddedMonospacedFont());

        connect(model, &ClientModel::numBlocksChanged, this, &OverviewPage::updateDisplayUnit);
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setLimit(NUM_ITEMS);
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        // Keep up to date with wallet
        setBalance(model->getCachedBalance());
        connect(model, &WalletModel::balanceChanged, this, &OverviewPage::setBalance);

        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &OverviewPage::updateDisplayUnit);

        interfaces::Wallet& wallet = model->wallet();
        updateWatchOnlyLabels(wallet.haveWatchOnly() && !wallet.privateKeysDisabled());
        connect(model, &WalletModel::notifyWatchonlyChanged, [this](bool showWatchOnly) {
            updateWatchOnlyLabels(showWatchOnly && !walletModel->wallet().privateKeysDisabled());
        });
    }

    // update the display unit, to not use the default ("CASH")
    updateDisplayUnit();
}

void OverviewPage::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
        ui->labelTransactionsStatus->setIcon(icon);
        ui->labelWalletStatus0->setIcon(icon);
        ui->labelWalletStatus1->setIcon(icon);
    }

    QWidget::changeEvent(e);
}

void OverviewPage::updateDisplayUnit()
{
    if (walletModel && walletModel->getOptionsModel()) {
        const auto& balances = walletModel->getCachedBalance();
        if (balances.cash.balance != -1 && balances.bond.balance != -1) {
            setBalance(balances);
        }

        // Update txdelegate->unit with the current unit
        BitcoinUnit cashUnit = walletModel->getOptionsModel()->getDisplayUnit(CASH);
        BitcoinUnit bondUnit = walletModel->getOptionsModel()->getDisplayUnit(BOND);
        txdelegate->cashUnit = cashUnit;
        txdelegate->bondUnit = bondUnit;

        ui->listTransactions->update();

        CAmount amountIn = (CAmount)BitcoinUnits::factor(bondUnit);
        if (walletModel->getOptionsModel()->getShowScaledAmount(BOND)) {
            amountIn = DescaleAmount(amountIn, walletModel->getBestScaleFactor());
        }
        CAmount conversionRate = walletModel->estimateConversionOutputAmount(amountIn, BOND);
        if (walletModel->getOptionsModel()->getShowScaledAmount(CASH)) {
            conversionRate = ScaleAmount(conversionRate, walletModel->getBestScaleFactor());
        }
        ui->labelConversionRate->setText("1 " + BitcoinUnits::shortName(bondUnit) + " ≈ " + BitcoinUnits::formatWithUnit(cashUnit, conversionRate));

        const int64_t interestRate = walletModel->getBestInterestRate();
        ui->labelInterestRate->setText(QString::number(interestRate / 100) + "." + QString::number(interestRate % 100).rightJustified(2, '0') + "%");
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus0->setVisible(fShow);
    ui->labelWalletStatus1->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::setMonospacedFont(bool use_embedded_font)
{
    QFont f = GUIUtil::fixedPitchFont(use_embedded_font);
    f.setWeight(QFont::Bold);
    ui->labelBalance0->setFont(f);
    ui->labelUnconfirmed0->setFont(f);
    ui->labelImmature0->setFont(f);
    ui->labelTotal0->setFont(f);
    ui->labelWatchAvailable0->setFont(f);
    ui->labelWatchPending0->setFont(f);
    ui->labelWatchImmature0->setFont(f);
    ui->labelWatchTotal0->setFont(f);

    ui->labelBalance1->setFont(f);
    ui->labelUnconfirmed1->setFont(f);
    ui->labelImmature1->setFont(f);
    ui->labelTotal1->setFont(f);
    ui->labelWatchAvailable1->setFont(f);
    ui->labelWatchPending1->setFont(f);
    ui->labelWatchImmature1->setFont(f);
    ui->labelWatchTotal1->setFont(f);

    ui->labelConversionRate->setFont(f);
    ui->labelInterestRate->setFont(f);
}
