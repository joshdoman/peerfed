// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_CONVERTCOINSDIALOG_H
#define BITCOIN_QT_CONVERTCOINSDIALOG_H

#include <qt/guiutil.h>
#include <qt/walletmodel.h>

#include <QDialog>
#include <QHeaderView>
#include <QItemSelection>
#include <QKeyEvent>
#include <QMenu>
#include <QPoint>
#include <QVariant>

class PlatformStyle;
class ClientModel;
class WalletModel;
namespace wallet {
class CCoinControl;
} // namespace wallet

namespace Ui {
    class ConvertCoinsDialog;
}

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

/** Dialog for requesting payment of bitcoins */
class ConvertCoinsDialog : public QDialog
{
    Q_OBJECT

public:
    double DEFAULT_SLIPPAGE = 0.5;

    explicit ConvertCoinsDialog(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~ConvertCoinsDialog();

    void setClientModel(ClientModel *clientModel);
    void setModel(WalletModel *model);

public Q_SLOTS:
    void updateConversionType();
    void onInputChanged();
    void onOutputChanged();
    void convertButtonClicked();
    void clear();
    void reject() override;
    void accept() override;

Q_SIGNALS:
    void coinsConverted(const uint256& txid);

private:
    Ui::ConvertCoinsDialog *ui;
    ClientModel *clientModel;
    WalletModel *model;
    QMenu *contextMenu;
    const PlatformStyle *platformStyle;

    std::unique_ptr<wallet::CCoinControl> m_coin_control;
    std::unique_ptr<WalletModelConversionTransaction> m_current_transaction;

    bool inputIsExact = true; // if false, output is exact
    bool calculatingInput = false;
    bool calculatingOutput = false;

    CAmountType getInputType();
    CAmountType getOutputType();

    // Copy PSBT to clipboard and offer to save it.
    void presentPSBT(PartiallySignedTransaction& psbt);

    // Process WalletModel::SendCoinsReturn and generate a pair consisting
    // of a message and message flags for use in Q_EMIT message().
    // Additional parameter msgArg can be used via .arg(msgArg).
    void processConvertCoinsReturn(const WalletModel::ConvertCoinsReturn &sendCoinsReturn, const QString &msgArg = QString());

    /* Sign PSBT using external signer.
     *
     * @param[in,out] psbtx the PSBT to sign
     * @param[in,out] mtx needed to attempt to finalize
     * @param[in,out] complete whether the PSBT is complete (a successfully signed multisig transaction may not be complete)
     *
     * @returns false if any failure occurred, which may include the user rejection of a transaction on the device.
     */
    bool signWithExternalSigner(PartiallySignedTransaction& psbt, CMutableTransaction& mtx, bool& complete);

private Q_SLOTS:
    void updateDisplayUnit();
    void recalculate();

Q_SIGNALS:
    // Fired when a message should be reported to the user
    void message(const QString &title, const QString &message, unsigned int style);
};

#endif // BITCOIN_QT_CONVERTCOINSDIALOG_H
