// Copyright (c) 2011-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_CONVERTCOINSDIALOG_H
#define BITCOIN_QT_CONVERTCOINSDIALOG_H

#include <qt/guiutil.h>

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
    void clear();
    void reject() override;
    void accept() override;

private:
    bool inputIsExact = true; // if false, output is exact
    bool calculatingInput = false;
    bool calculatingOutput = false;
    Ui::ConvertCoinsDialog *ui;
    ClientModel *clientModel;
    WalletModel *model;
    QMenu *contextMenu;
    const PlatformStyle *platformStyle;

    CAmountType getInputType();
    CAmountType getOutputType();

private Q_SLOTS:
    void updateDisplayUnit();
    void recalculate();
};

#endif // BITCOIN_QT_CONVERTCOINSDIALOG_H
