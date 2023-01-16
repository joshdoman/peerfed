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
    enum ColumnWidths {
        DATE_COLUMN_WIDTH = 130,
        LABEL_COLUMN_WIDTH = 120,
        AMOUNT_MINIMUM_COLUMN_WIDTH = 180,
        MINIMUM_COLUMN_WIDTH = 130
    };

    explicit ConvertCoinsDialog(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~ConvertCoinsDialog();

    void setModel(WalletModel *model);

public Q_SLOTS:
    void clear();
    void reject() override;
    void accept() override;

private:
    Ui::ConvertCoinsDialog *ui;
    WalletModel *model;
    QMenu *contextMenu;
    const PlatformStyle *platformStyle;

private Q_SLOTS:
    void updateDisplayUnit();
};

#endif // BITCOIN_QT_CONVERTCOINSDIALOG_H
