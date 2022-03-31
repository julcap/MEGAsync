#include "NodeSelector.h"
#include "ui_NodeSelector.h"
#include "ui_NewFolderDialog.h"
#include "MegaApplication.h"
#include "QMegaMessageBox.h"
#include "control/Utilities.h"
#include "MegaItemProxyModel.h"
#include "MegaItemModel.h"
#include "megaapi.h"
#include "MegaItemDelegates.h"
#include "mega/utils.h"

#include <QMessageBox>
#include <QPointer>
#include <QMenu>
#include <QShortcut>

using namespace mega;

// Human-friendly list of forbidden chars for New Remote Folder
static const QString forbidden(QString::fromLatin1("\\ / : \" * < > \? |"));
// Forbidden chars PCRE using a capture list: [\\/:"\*<>?|]
static const QRegularExpression forbiddenRx(QString::fromLatin1("[\\\\/:\"*<>\?|]"));
// Time to show the new remote folder input error
static int newFolderErrorDisplayTime = 10000; //10s in milliseconds

const int NodeSelector::LABEL_ELIDE_MARGIN = 100;


NodeSelector::NodeSelector(int selectMode, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::NodeSelector),
    mNewFolderUi(new Ui::NewFolderDialog),
    mNewFolder(new QDialog(this)),
    mSelectMode(selectMode),
    mMegaApi(static_cast<MegaApplication*>(qApp)->getMegaApi()),
    mDelegateListener(mega::make_unique<QTMegaRequestListener>(static_cast<MegaApplication*>(qApp)->getMegaApi(), this)),
    mModel(nullptr)
{
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    setWindowModality(Qt::WindowModal);
    ui->setupUi(this);

    ui->cbAlwaysUploadToLocation->hide();
    ui->bOk->setDefault(true);

#ifndef Q_OS_MAC
    ui->bShowCloudDrive->setChecked(true);
    connect(ui->bShowIncomingShares, &QPushButton::clicked, this, &NodeSelector::onbShowIncomingSharesClicked);
    connect(ui->bShowCloudDrive, &QPushButton::clicked,this , &NodeSelector::onbShowCloudDriveClicked);
#else
    ui->tabBar->addTab(tr("Cloud Drive"));
    ui->tabBar->addTab(tr("Incoming Shares"));
    connect(ui->tabBar, &QTabBar::currentChanged, this, &NodeSelector::onTabSelected);
#endif

    nodesReady();

    ui->tMegaFolders->setContextMenuPolicy(Qt::DefaultContextMenu);
    ui->tMegaFolders->setExpandsOnDoubleClick(false);
    ui->tMegaFolders->setSortingEnabled(true);
    ui->tMegaFolders->header()->setFixedHeight(MegaItemModel::ROW_HEIGHT);
    ui->tMegaFolders->header()->moveSection(MegaItemModel::STATUS, MegaItemModel::NODE);
    ui->tMegaFolders->header()->setProperty("HeaderIconCenter", true);
    ui->tMegaFolders->setColumnWidth(MegaItemModel::COLUMN::STATUS, MegaItemModel::ROW_HEIGHT * 2);
    ui->tMegaFolders->setItemDelegate(new  NodeRowDelegate(ui->tMegaFolders));
    ui->tMegaFolders->setItemDelegateForColumn(MegaItemModel::STATUS, new IconDelegate(ui->tMegaFolders));
    ui->tMegaFolders->setExpanded(mProxyModel->getIndexFromHandle(mMegaApi->getRootNode()->getHandle()),true);
    ui->tMegaFolders->setTextElideMode(Qt::ElideMiddle);

    ui->lFolderName->setText(tr("Cloud Drive"));


    connect(ui->tMegaFolders->selectionModel(), &QItemSelectionModel::selectionChanged, this, &NodeSelector::onSelectionChanged);
    connect(ui->tMegaFolders, &MegaItemTreeView::removeNodeClicked, this, &NodeSelector::onDeleteClicked);
    connect(ui->tMegaFolders, &MegaItemTreeView::getMegaLinkClicked, this, &NodeSelector::onGenMEGALinkClicked);
    connect(ui->tMegaFolders, &QTreeView::doubleClicked, this, &NodeSelector::onItemDoubleClick);
    connect(ui->bForward, &QPushButton::clicked, this, &NodeSelector::onGoForwardClicked);
    connect(ui->bBack, &QPushButton::clicked, this, &NodeSelector::onGoBackClicked);
    connect(ui->bNewFolder, &QPushButton::clicked, this, &NodeSelector::onbNewFolderClicked);
    connect(ui->bOk, &QPushButton::clicked, this, &NodeSelector::onbOkClicked);
    connect(ui->bCancel, &QPushButton::clicked, this, &QDialog::reject);


    // Provide quick access shortcuts for the two panes via Ctrl+1,2
    // Ctrl is auto-magically translated to CMD key by Qt on macOS
    for (int i = 0; i < 2; ++i)
    {
        QShortcut *shortcut = new QShortcut(QKeySequence(QString::fromLatin1("Ctrl+%1").arg(i+1)), this);
        QObject::connect(shortcut, &QShortcut::activated, this, [=](){ onTabSelected(i); });
    }

    setupNewFolderDialog();
}

NodeSelector::~NodeSelector()
{
    delete ui;
    delete mNewFolderUi;
    delete mNewFolder;
}

void NodeSelector::nodesReady()
{
    if (!mMegaApi->isFilesystemAvailable())
    {
        ui->bOk->setEnabled(false);
        ui->bNewFolder->setEnabled(false);
        return;
    }

    mModel = mega::make_unique<MegaItemModel>(this);
    mProxyModel = mega::make_unique<MegaItemProxyModel>(this);
    mProxyModel->setSourceModel(mModel.get());
    switch(mSelectMode)
    {
    case NodeSelector::SYNC_SELECT:
        mModel->setSyncSetupMode(true);
        mProxyModel->showReadWriteFolders(false);
        // fall through
    case NodeSelector::UPLOAD_SELECT:
        mProxyModel->showReadOnlyFolders(false);
        mModel->showFiles(false);
        ui->bNewFolder->show();
        break;
    case NodeSelector::DOWNLOAD_SELECT:
        ui->bNewFolder->hide();
        mModel->showFiles(true);
        ui->tMegaFolders->setSelectionMode(QAbstractItemView::ExtendedSelection);
        break;
    case NodeSelector::STREAM_SELECT:
        mModel->showFiles(true);
        ui->bNewFolder->hide();
        setWindowTitle(tr("Select items"));
        break;
    }    
    ui->tMegaFolders->setModel(mProxyModel.get());
    ui->tMegaFolders->sortByColumn(MegaItemModel::NODE, Qt::AscendingOrder);
    checkBackForwardButtons();

//Disable animation for OS X due to problems showing the tree icons
#ifdef __APPLE__
    ui->tMegaFolders->setAnimated(false);
#endif
}

void NodeSelector::showEvent(QShowEvent* )
{
    ui->tMegaFolders->setColumnWidth(MegaItemModel::COLUMN::NODE, qRound(ui->tMegaFolders->width() * 0.6));
}

void NodeSelector::resizeEvent(QResizeEvent *)
{
    ui->tMegaFolders->setColumnWidth(MegaItemModel::COLUMN::NODE, qRound(ui->tMegaFolders->width() * 0.6));
}

void NodeSelector::mousePressEvent(QMouseEvent *event)
{
    if(event->button() == Qt::BackButton && ui->bBack->isEnabled())
    {
       onGoBackClicked();
    }
    else if(event->button() == Qt::ForwardButton && ui->bForward->isEnabled())
    {
       onGoForwardClicked();
    }
}

void NodeSelector::showDefaultUploadOption(bool show)
{
    ui->cbAlwaysUploadToLocation->setVisible(show);
}

void NodeSelector::setDefaultUploadOption(bool value)
{
    ui->cbAlwaysUploadToLocation->setChecked(value);
}

MegaHandle NodeSelector::getSelectedNodeHandle()
{
    return ui->tMegaFolders->getSelectedNodeHandle();
}

QList<MegaHandle> NodeSelector::getMultiSelectionNodeHandle()
{
    QList<MegaHandle> ret;
    foreach(auto& s_index, ui->tMegaFolders->selectionModel()->selectedRows())
    {
        if(auto node = mProxyModel->getNode(s_index))
            ret.append(node->getHandle());
    }
    return ret;
}

QModelIndex NodeSelector::getSelectedIndex()
{
    QModelIndex ret;
    if(ui->tMegaFolders->selectionModel()->selectedRows().size() > 0)
        ret = ui->tMegaFolders->selectionModel()->selectedRows().at(0);
    return ret;
}

void NodeSelector::setSelectedNodeHandle(MegaHandle selectedHandle)
{
    auto node = std::shared_ptr<MegaNode>(mMegaApi->getNodeByHandle(selectedHandle));
    if (!node)
        return;
    std::shared_ptr<MegaNode> root_p_node = node;

    while(root_p_node)
    {
        if(auto p_node = std::shared_ptr<MegaNode>(mMegaApi->getParentNode(root_p_node.get())))
            root_p_node = p_node;
        else
            break;
    }

    bool isInShare = false;
    if( root_p_node && root_p_node->isInShare())
    {
        isInShare=true;
        onbShowIncomingSharesClicked();
    }

    QVector<QModelIndex> modelIndexList = mProxyModel->getRelatedModelIndexes(node, isInShare);

    if(modelIndexList.size() > 1)
    {
        foreach(QModelIndex idx, modelIndexList)
        {
            ui->tMegaFolders->expand(idx);
        }
    }

    if(modelIndexList.size() > 0)
    {
        ui->tMegaFolders->selectionModel()->setCurrentIndex(modelIndexList.last(), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows	);
        ui->tMegaFolders->selectionModel()->select(modelIndexList.last(), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
}

void NodeSelector::onRequestFinish(MegaApi *, MegaRequest *request, MegaError *e)
{
    ui->bNewFolder->setEnabled(true);
    ui->bOk->setEnabled(true);

    if (e->getErrorCode() != MegaError::API_OK)
    {
        ui->tMegaFolders->setEnabled(true);
        QMegaMessageBox::critical(nullptr, QString::fromUtf8("MEGAsync"), tr("Error") + QString::fromUtf8(": ") + QCoreApplication::translate("MegaError", e->getErrorString()));
        return;
    }

    if(request->getType() == MegaRequest::TYPE_CREATE_FOLDER)
    {
        if (e->getErrorCode() == MegaError::API_OK)
        {
            auto node = mMegaApi->getNodeByHandle(request->getNodeHandle());
            auto nodeUnique = std::unique_ptr<MegaNode>(node);
            if (nodeUnique)
            {
                QModelIndex idx = ui->tMegaFolders->rootIndex();
                if(!idx.isValid())
                {
                    idx = mProxyModel->getIndexFromNode(((MegaApplication*)qApp)->getRootNode());
                }
                QModelIndex row = mProxyModel->insertNode(std::move(nodeUnique), idx);
                ui->tMegaFolders->selectionModel()->select(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                ui->tMegaFolders->selectionModel()->setCurrentIndex(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            }
        }
    }
    else if (request->getType() == MegaRequest::TYPE_REMOVE || request->getType() == MegaRequest::TYPE_MOVE)
    {
        if (e->getErrorCode() == MegaError::API_OK)
        {
            auto selectedIndex = getSelectedIndex();
            if(!selectedIndex.isValid())
              return;

            auto parent = mProxyModel->getNode(selectedIndex.parent());
            mNavCloudDrive.remove(mProxyModel->getHandle(selectedIndex));
            mProxyModel->removeNode(selectedIndex);
            if(parent)
                setSelectedNodeHandle(parent->getHandle());
        }
    }
    checkBackForwardButtons();
    ui->tMegaFolders->setEnabled(true);
}

void NodeSelector::onDeleteClicked()
{
    MegaNode *node = mMegaApi->getNodeByHandle(getSelectedNodeHandle());
    int access = mMegaApi->getAccess(node);
    if (!node || access < MegaShare::ACCESS_FULL)
    {
        delete node;
        return;
    }

    QPointer<NodeSelector> currentDialog = this;
    if (QMegaMessageBox::question(this,
                             QString::fromUtf8("MEGAsync"),
                             tr("Are you sure that you want to delete \"%1\"?")
                                .arg(QString::fromUtf8(node->getName())),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
    {
        if (!currentDialog)
        {
            delete node;
            return;
        }

        ui->tMegaFolders->setEnabled(false);
        ui->bBack->setEnabled(false);
        ui->bForward->setEnabled(false);
        ui->bNewFolder->setEnabled(false);
        ui->bOk->setEnabled(false);
        const char *name = node->getName();
        if (access == MegaShare::ACCESS_FULL
                || !strcmp(name, "NO_KEY")
                || !strcmp(name, "CRYPTO_ERROR")
                || !strcmp(name, "BLANK"))
        {
            mMegaApi->remove(node, mDelegateListener.get());
        }
        else
        {
            auto rubbish = ((MegaApplication*)qApp)->getRubbishNode();
            mMegaApi->moveNode(node, rubbish.get(), mDelegateListener.get());
        }
    }
    delete node;
}

void NodeSelector::onGenMEGALinkClicked()
{
    MegaNode *node = mMegaApi->getNodeByHandle(getSelectedNodeHandle());
    if (!node || node->getType() == MegaNode::TYPE_ROOT
            || mMegaApi->getAccess(node) != MegaShare::ACCESS_OWNER)
    {
        delete node;
        return;
    }

    mMegaApi->exportNode(node);
    delete node;
}

void NodeSelector::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
    {
        ui->retranslateUi(this);
        mNewFolderUi->retranslateUi(mNewFolder);
        mNewFolderUi->errorLabel->setText(mNewFolderUi->errorLabel->text().arg(forbidden));
        nodesReady();
    }
    QDialog::changeEvent(event);
}

void NodeSelector::onItemDoubleClick(const QModelIndex &index)
{
    if(!isAllowedToEnterInIndex(index) )
        return;

    if(isCloudDrive())
    {
        mNavCloudDrive.appendToBackward(getHandleByIndex(ui->tMegaFolders->rootIndex()));
        mNavCloudDrive.removeFromForward(mProxyModel->getHandle(index));
    }
    else
    {
        mNavInShares.appendToBackward(getHandleByIndex(ui->tMegaFolders->rootIndex()));
        mNavInShares.removeFromForward(mProxyModel->getHandle(index));
    }

    setRootIndex(index);
    checkBackForwardButtons();
    checkNewFolderButtonVisibility();
}

void NodeSelector::onGoBackClicked()
{
    QModelIndex indexToGo;
    if(isCloudDrive())
    {
        mNavCloudDrive.appendToForward(getHandleByIndex(ui->tMegaFolders->rootIndex()));
        indexToGo = getIndexFromHandle(mNavCloudDrive.backwardHandles.last());
        mNavCloudDrive.backwardHandles.removeLast();
    }
    else
    {
        mNavInShares.appendToForward(getHandleByIndex(ui->tMegaFolders->rootIndex()));
        indexToGo = getIndexFromHandle(mNavInShares.backwardHandles.last());
        mNavInShares.backwardHandles.removeLast();
    }
    setRootIndex(indexToGo);
    checkBackForwardButtons();
    checkNewFolderButtonVisibility();
}

void NodeSelector::onGoForwardClicked()
{
    QModelIndex indexToGo;
    if(isCloudDrive())
    {
        mNavCloudDrive.appendToBackward(getHandleByIndex(ui->tMegaFolders->rootIndex()));
        indexToGo = getIndexFromHandle(mNavCloudDrive.forwardHandles.last());
        mNavCloudDrive.forwardHandles.removeLast();
    }
    else
    {
        mNavInShares.appendToBackward(getHandleByIndex(ui->tMegaFolders->rootIndex()));
        indexToGo = getIndexFromHandle(mNavInShares.forwardHandles.last());
        mNavInShares.forwardHandles.removeLast();
    }
    setRootIndex(indexToGo);
    checkBackForwardButtons();
    checkNewFolderButtonVisibility();
}

void NodeSelector::onbNewFolderClicked()
{
    mNewFolderUi->errorLabel->hide();
    mNewFolderUi->textLabel->show();
    mNewFolderUi->lineEdit->clear();
    mNewFolderUi->lineEdit->setFocus();
    if (!mNewFolder->exec())
    {
        //dialog rejected, cancel New Folder operation
        return;
    }

    QString newFolderName = mNewFolderUi->lineEdit->text().trimmed();
    auto parentNode = mProxyModel->getNode(ui->tMegaFolders->rootIndex());
    if (!parentNode)
    {
        auto rootNode = ((MegaApplication*)qApp)->getRootNode();
        if (rootNode)
            parentNode = std::shared_ptr<MegaNode>(rootNode);

        if (!parentNode)
            return;
    }

    auto node = std::unique_ptr<MegaNode>(mMegaApi->getNodeByPath(newFolderName.toUtf8().constData(), parentNode.get()));
    if (!node || node->isFile())
    {
        ui->bNewFolder->setEnabled(false);
        ui->bOk->setEnabled(false);
        ui->tMegaFolders->setEnabled(false);
        ui->bBack->setEnabled(false);
        ui->bForward->setEnabled(false);
        mMegaApi->createFolder(newFolderName.toUtf8().constData(), parentNode.get(), mDelegateListener.get());
    }
    else
    {
        auto modelIndex = mProxyModel->getIndexFromNode(parentNode);
        for (int i = 0; i < mProxyModel->rowCount(modelIndex); i++)
        {
            QModelIndex row = mProxyModel->index(i, 0, modelIndex);
            auto node = mProxyModel->getNode(row);

            if (node && newFolderName.compare(QString::fromUtf8(node->getName())) == 0)
            {
                setSelectedNodeHandle(node->getHandle());
                ui->tMegaFolders->selectionModel()->select(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                ui->tMegaFolders->selectionModel()->setCurrentIndex(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                break;
            }
        }
    }
}

void NodeSelector::onbOkClicked()
{
    MegaNode *node = mMegaApi->getNodeByHandle(getSelectedNodeHandle());
    if (!node)
    {
        reject();
        return;
    }

    int access = mMegaApi->getAccess(node);
    if ((mSelectMode == NodeSelector::UPLOAD_SELECT) && ((access < MegaShare::ACCESS_READWRITE)))
    {
        delete node;
        QMegaMessageBox::warning(nullptr, tr("Error"), tr("You need Read & Write or Full access rights to be able to upload to the selected folder."), QMessageBox::Ok);
        return;

    }
    else if ((mSelectMode == NodeSelector::SYNC_SELECT) && (access < MegaShare::ACCESS_FULL))
    {
        delete node;
        QMegaMessageBox::warning(nullptr, tr("Error"), tr("You need Full access right to be able to sync the selected folder."), QMessageBox::Ok);
        return;
    }
    else if ((mSelectMode == NodeSelector::STREAM_SELECT) && node->isFolder())
    {
        delete node;
        QMegaMessageBox::warning(nullptr, tr("Error"), tr("Only files can be used for streaming."), QMessageBox::Ok);
        return;
    }

    if (mSelectMode == NodeSelector::SYNC_SELECT)
    {
        const char* path = mMegaApi->getNodePath(node);
        MegaNode *check = mMegaApi->getNodeByPath(path);
        delete [] path;
        if (!check)
        {
            delete node;
            QMegaMessageBox::warning(nullptr, tr("Warning"), tr("Invalid folder for synchronization.\n"
                                                         "Please, ensure that you don't use characters like '\\' '/' or ':' in your folder names."),
                                 QMessageBox::Ok);
            return;
        }
        delete check;
    }

    delete node;
    accept();
}

void NodeSelector::onbShowIncomingSharesClicked()
{
    if(mProxyModel)
    {
        saveExpandedItems();
        mProxyModel->showOnlyInShares();
        restoreExpandedItems();
        checkNewFolderButtonVisibility();
        checkBackForwardButtons();
    }
}

void NodeSelector::onbShowCloudDriveClicked()
{
    if(mProxyModel)
    {
        saveExpandedItems();
        mProxyModel->showOnlyCloudDrive();
        restoreExpandedItems();
        checkNewFolderButtonVisibility();
        checkBackForwardButtons();
    }
}

void NodeSelector::onTabSelected(int index)
{
    switch (index)
    {
        case NodeSelector::CLOUD_DRIVE:
#ifdef Q_OS_MAC
            onbShowCloudDriveClicked();
            ui->tabBar->setCurrentIndex(index);
#else
            ui->bShowCloudDrive->click();
#endif
            break;
        case NodeSelector::SHARES:
#ifdef Q_OS_MAC
            onbShowIncomingSharesClicked();
            ui->tabBar->setCurrentIndex(index);
#else
            ui->bShowIncomingShares->click();
#endif
            break;
        default:
            break;
    }
}

void NodeSelector::onSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
    Q_UNUSED(deselected)
    if(mSelectMode == UPLOAD_SELECT || mSelectMode == DOWNLOAD_SELECT || !mProxyModel)
    {
        return;
    }
    foreach(auto& index, selected.indexes())
    {
        auto source_idx = mProxyModel->getIndexFromSource(index);
        MegaItem *item = static_cast<MegaItem*>(source_idx.internalPointer());
        if(item)
        {
            if(mSelectMode == NodeSelector::STREAM_SELECT)
                ui->bOk->setEnabled(item->getNode()->isFile());
            else if(mSelectMode == NodeSelector::SYNC_SELECT)
                ui->bOk->setEnabled(item->isSyncable());
        }
    }
}

void NodeSelector::saveExpandedItems()
{
    auto node = mProxyModel->getNode(ui->tMegaFolders->rootIndex());

    if(isCloudDrive())
    {
        mNavCloudDrive.rootHandle = node? node->getHandle() : INVALID_HANDLE;
        iterateForSaveExpanded(mNavCloudDrive.expandedHandles);
    }
    else
    {
        mNavInShares.rootHandle  = node? node->getHandle() : INVALID_HANDLE;
        iterateForSaveExpanded(mNavInShares.expandedHandles);
    }
}

void NodeSelector::iterateForSaveExpanded(QList<MegaHandle> &saveList, const QModelIndex& parent)
{
    for(int i=0; i < mProxyModel->rowCount(parent); ++i)
    {
        auto idx = mProxyModel->index(i, 0, parent);
        if(idx.isValid() && ui->tMegaFolders->isExpanded(idx))
        {
            saveList.append(mProxyModel->getNode(idx)->getHandle());
            iterateForSaveExpanded(saveList, idx);
        }
    }
}

void NodeSelector::restoreExpandedItems()
{
    if(isCloudDrive())
    {
        auto idx = mProxyModel->getIndexFromHandle(mNavCloudDrive.rootHandle);
        setRootIndex(idx);
        iterateForRestore(mNavCloudDrive.expandedHandles);
        mNavCloudDrive.expandedHandles.clear();
    }
    else
    {
        auto idx = mProxyModel->getIndexFromHandle(mNavInShares.rootHandle);
        setRootIndex(idx);
        iterateForRestore(mNavInShares.expandedHandles);
        mNavInShares.expandedHandles.clear();
    }

}

void NodeSelector::iterateForRestore(const QList<MegaHandle> &list, const QModelIndex &parent)
{
    if(list.isEmpty())
        return;

    for(int i=0; i < mProxyModel->rowCount(parent); ++i)
    {
        auto idx = mProxyModel->index(i, 0, parent);
        if(idx.isValid() && list.contains(mProxyModel->getNode(idx)->getHandle()))
        {
           ui->tMegaFolders->expand(idx);
           iterateForRestore(list, idx);
        }
    }
}

bool NodeSelector::isAllowedToEnterInIndex(const QModelIndex &idx)
{
    auto source_idx = mProxyModel->getIndexFromSource(idx);
    MegaItem *item = static_cast<MegaItem*>(source_idx.internalPointer());
    if(item)
    {
        if((item->getNode()->isFile())
           || (item->isRoot())
           || (mSelectMode == NodeSelector::SYNC_SELECT && (item->getStatus() == MegaItem::SYNC || item->getStatus() == MegaItem::SYNC_CHILD)))
        {
            return false;
        }
    }
    return true;
}

bool NodeSelector::isCloudDrive()
{
    if(mProxyModel)
        return mProxyModel->isShowOnlyCloudDrive();

    return true;
}

void NodeSelector::setRootIndex(const QModelIndex &idx)
{
    ui->tMegaFolders->setRootIndex(idx);
    if(!idx.isValid())
    {
        if(isCloudDrive())
            ui->lFolderName->setText(tr("Cloud Drive"));
        else
            ui->lFolderName->setText(tr("Incoming Shares"));

        ui->lFolderName->setToolTip(QString());

        ui->lIcon->setPixmap(QPixmap());
        return;
    }
    auto source_idx = mProxyModel->getIndexFromSource(idx);
    if(!source_idx.isValid())
    {
        ui->lIcon->setPixmap(QPixmap());
        return;
    }
    if(source_idx.column() != MegaItemModel::COLUMN::STATUS)
    {
        source_idx = source_idx.sibling(source_idx.row(), MegaItemModel::COLUMN::STATUS);
    }
    QIcon icon = qvariant_cast<QIcon>(source_idx.data(Qt::DecorationRole));

    MegaItem *item = static_cast<MegaItem*>(source_idx.internalPointer());
    if(!item)
        return;

    if(!icon.isNull())
    {
        QFontMetrics fm(font());
        QPixmap pm = icon.pixmap(QSize(fm.height()+3, fm.height()+3), QIcon::Normal);
        ui->lIcon->setPixmap(pm);
    }
    else
    {
        ui->lIcon->setPixmap(QPixmap());
    }
    auto node = item->getNode();
    if(node)
    {
        QString nodeName = QString::fromUtf8(node->getName());
        QFontMetrics fm = ui->lFolderName->fontMetrics();
        ui->lFolderName->setText(nodeName);

        QString elidedText = fm.elidedText(nodeName, Qt::ElideMiddle, ui->tMegaFolders->width() - LABEL_ELIDE_MARGIN);
        ui->lFolderName->setText(elidedText);

        if(elidedText != nodeName)
            ui->lFolderName->setToolTip(nodeName);
        else
            ui->lFolderName->setToolTip(QString());

    }

}

MegaHandle NodeSelector::getHandleByIndex(const QModelIndex& idx)
{
    if(mProxyModel)
        return mProxyModel->getHandle(idx);

    return mega::INVALID_HANDLE;
}

QModelIndex NodeSelector::getIndexFromHandle(const mega::MegaHandle &handle)
{
    if(mProxyModel)
        return mProxyModel->getIndexFromHandle(handle);

    return QModelIndex();
}

void NodeSelector::checkBackForwardButtons()
{
    bool enableBackward(false);
    bool enableForward(false);
    if(isCloudDrive())
    {
        enableBackward = !mNavCloudDrive.backwardHandles.isEmpty();
        enableForward = !mNavCloudDrive.forwardHandles.isEmpty();
    }
    else
    {
        enableBackward = !mNavInShares.backwardHandles.isEmpty();
        enableForward = !mNavInShares.forwardHandles.isEmpty();
    }
    ui->bBack->setEnabled(enableBackward);
    ui->bForward->setEnabled(enableForward);
}

void NodeSelector::checkNewFolderButtonVisibility()
{
    if(mSelectMode == SYNC_SELECT || mSelectMode == UPLOAD_SELECT)
    {
        auto sourceIndex = mProxyModel->getIndexFromSource(ui->tMegaFolders->rootIndex());
        if(!sourceIndex.isValid() && !isCloudDrive())
        {
            ui->bNewFolder->hide();
            return;
        }
        ui->bNewFolder->show();
    }
}


void NodeSelector::setupNewFolderDialog()
{
    // Initialize the mNewFolder input Dialog
    mNewFolder->setWindowFlags(mNewFolder->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    mNewFolderUi->setupUi(mNewFolder);

    mNewFolderUi->errorLabel->setText(mNewFolderUi->errorLabel->text().arg(forbidden));
    // The dialog doesn't get resized on error
    mNewFolderUi->textLabel->setMinimumSize(mNewFolderUi->errorLabel->sizeHint());

    connect(mNewFolderUi->buttonBox, &QDialogButtonBox::rejected, mNewFolder, &QDialog::reject);
    QPushButton *okButton = mNewFolderUi->buttonBox->button(QDialogButtonBox::Ok);
    //only enabled when there's input, guards against empty folder name
    okButton->setEnabled(false);
    connect(mNewFolderUi->lineEdit, &QLineEdit::textChanged, this, [this, okButton]()
    {
        bool hasText = !mNewFolderUi->lineEdit->text().trimmed().isEmpty();
        okButton->setEnabled(hasText);
    });

    mNewFolderErrorTimer.setSingleShot(true);
    connect(&mNewFolderErrorTimer, &QTimer::timeout, this, [this]()
    {
        Utilities::animateFadeout(mNewFolderUi->errorLabel);
        // after animation is finished, hide the error label and show the original text
        // 700 magic number is how long Utilities::animateFadeout takes
        QTimer::singleShot(700, this, [this]()
        {
            mNewFolderUi->errorLabel->hide();
            mNewFolderUi->textLabel->show();
        });
    });
    connect(mNewFolderUi->buttonBox, &QDialogButtonBox::accepted, this, [this]
    {
        if(mNewFolderUi->lineEdit->text().trimmed().contains(forbiddenRx))
        {
            // show error label, dialog stays open
            mNewFolderUi->textLabel->hide();
            mNewFolderUi->errorLabel->show();
            Utilities::animateFadein(mNewFolderUi->errorLabel);
            mNewFolderErrorTimer.start(newFolderErrorDisplayTime); //(re)start timer
            mNewFolderUi->lineEdit->setFocus();
        }
        else
        {
            //dialog accepted, execute New Folder operation
            mNewFolder->accept();
        }
    });
}

bool NodeSelector::getDefaultUploadOption()
{
   return ui->cbAlwaysUploadToLocation->isChecked();
}

void NodeSelector::Navigation::removeFromForward(const mega::MegaHandle &handle)
{
    if(forwardHandles.size() == 0)
        return;

    auto megaApi = static_cast<MegaApplication*>(qApp)->getMegaApi();
    auto node = megaApi->getNodeByHandle(handle);
    if(!node)
        return;

    auto parentNode = megaApi->getParentNode(node);
    if(!parentNode)
        return;

    //if we are visiting common parent node we only have to store the new one and remove the old
    for(auto it = forwardHandles.begin(); it != forwardHandles.end(); ++it)
    {
        auto lastForwardNode = megaApi->getNodeByHandle(*it);
        if(!lastForwardNode)
           return;

        auto brotherParentNode = megaApi->getParentNode(lastForwardNode);
        if(!brotherParentNode)
          return;

        if(brotherParentNode->getHandle() == parentNode->getHandle())
        {
            forwardHandles.erase(it, forwardHandles.end());
            return;
        }
    }
}

void NodeSelector::Navigation::remove(const mega::MegaHandle &handle)
{
    backwardHandles.removeAll(handle);
    forwardHandles.removeAll(handle);
}

void NodeSelector::Navigation::appendToBackward(const mega::MegaHandle& handle)
{
    if(backwardHandles.isEmpty() || backwardHandles.last() != handle)
    {
        for(auto it = backwardHandles.begin(); it != backwardHandles.end();++it)
        {
            if(*it == handle)
            {
              backwardHandles.erase(it, backwardHandles.end());
              break;
            }
        }
        backwardHandles.append(handle);
    }
}

void NodeSelector::Navigation::appendToForward(const mega::MegaHandle &handle)
{
    if(forwardHandles.isEmpty() || forwardHandles.last() != handle)
        forwardHandles.append(handle);
}
