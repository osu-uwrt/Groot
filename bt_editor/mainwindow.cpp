#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDebug>
#include <QSettings>
#include <QTextStream>
#include <QList>
#include <QMap>
#include <QMessageBox>
#include <QFileDialog>
#include <QMenu>
#include <QToolButton>
#include <QWidgetAction>
#include <QTreeWidgetItem>
#include <QSvgWidget>
#include <QShortcut>
#include <QTabBar>
#include <QXmlStreamWriter>
#include <QDesktopServices>
#include <QInputDialog>
#include <nodes/Node>
#include <nodes/NodeData>
#include <nodes/NodeStyle>
#include <nodes/FlowView>

#include "editor_flowscene.h"
#include "utils.h"
#include "XML_utilities.hpp"

#include "models/RootNodeModel.hpp"
#include "models/SubtreeNodeModel.hpp"

#include "utils.h"

#include "ui_about_dialog.h"

#define workspace_dir(directory_path) tr("%1/.groot").arg(directory_path)
#define workspace_path(directory_path) tr("%1/workspace.xml").arg(workspace_dir(directory_path))

using QtNodes::DataModelRegistry;
using QtNodes::FlowView;
using QtNodes::FlowScene;
using QtNodes::NodeGraphicsObject;
using QtNodes::NodeState;

MainWindow::MainWindow(GraphicMode initial_mode, QWidget *parent) :
                                                                    QMainWindow(parent),
                                                                    ui(new Ui::MainWindow),
                                                                    _current_mode(initial_mode),
                                                                    _current_layout(QtNodes::PortLayout::Vertical)
{
    ui->setupUi(this);

    QSettings settings;
    restoreGeometry(settings.value("MainWindow/geometry").toByteArray());
    restoreState(settings.value("MainWindow/windowState").toByteArray());

    const QString layout = settings.value("MainWindow/layout").toString();
    if( layout == "HORIZONTAL")
    {
        _current_layout = QtNodes::PortLayout::Horizontal;
    }
    else{
        _current_layout = QtNodes::PortLayout::Vertical;
    }

    _model_registry = std::make_shared<QtNodes::DataModelRegistry>();

    //------------------------------------------------------

    auto registerModel = [this](const QString& ID, const NodeModel& model)
    {
        QString category = QString::fromStdString( BT::toStr(model.type) );
        if( ID == "Root")
        {
            category = "Root";
        }
        QtNodes::DataModelRegistry::RegistryItemCreator creator;
        creator = [model]() -> QtNodes::DataModelRegistry::RegistryItemPtr
        {
            auto ptr = new BehaviorTreeDataModel( model );
            return std::unique_ptr<BehaviorTreeDataModel>(ptr);
        };
        _model_registry->registerModel( category, creator, ID );
    };

    for(const auto& model: BuiltinNodeModels())
    {
        registerModel( model.first, model.second );
        _treenode_models.insert( { model.first, model.second } );
        qDebug() << "adding model: " << model.first;
    }
    //------------------------------------------------------

    _editor_widget = new SidepanelEditor(_model_registry.get(), _treenode_models, _workspace_models, this);
    _replay_widget = new SidepanelReplay(this);

    ui->leftFrame->layout()->addWidget( _editor_widget );
    ui->leftFrame->layout()->addWidget( _replay_widget );

#ifdef ZMQ_FOUND
    _monitor_widget = new SidepanelMonitor(this);
    ui->leftFrame->layout()->addWidget( _monitor_widget );

    connect( ui->toolButtonConnect, &QToolButton::clicked,
            _monitor_widget, &SidepanelMonitor::on_Connect );

    connect( _monitor_widget, &SidepanelMonitor::connectionUpdate,
            this, &MainWindow::onConnectionUpdate );
#else
    ui->actionMonitor_mode->setVisible(false);
#endif

    updateCurrentMode();

    dynamic_cast<QVBoxLayout*>(ui->leftFrame->layout())->setStretch(1,1);

    auto arrange_shortcut = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_A), this);

    connect( arrange_shortcut, &QShortcut::activated,
            this,   &MainWindow::onAutoArrange  );

    ui->splitter->setStretchFactor(0, 1);
    ui->splitter->setStretchFactor(1, 4);

    QShortcut* undo_shortcut = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Z), this);
    connect( undo_shortcut, &QShortcut::activated, this, &MainWindow::onUndoInvoked );

    QShortcut* redo_shortcut = new QShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Z), this);
    connect( redo_shortcut, &QShortcut::activated, this, &MainWindow::onRedoInvoked );

    connect( _editor_widget, &SidepanelEditor::nodeModelEdited,
            this, &MainWindow::onTreeNodeEdited);

    connect( _editor_widget, &SidepanelEditor::addNewModel,
            this, &MainWindow::onAddToModelRegistry);

    connect( _editor_widget, &SidepanelEditor::destroySubtree,
            this, &MainWindow::onDestroySubTree);

    connect( _editor_widget, &SidepanelEditor::modelRemoveRequested,
            this, &MainWindow::onModelRemoveRequested);

    connect( _editor_widget, &SidepanelEditor::addSubtree,
             this, [this](QString ID)
    {
        this->createTab(ID);
    });

    connect( _editor_widget, &SidepanelEditor::renameSubtree,
             this, [this](QString prev_ID, QString new_ID)
    {
        if (prev_ID == new_ID)
            return;
            
        for (int index = 0; index < ui->tabWidget->count(); index++)
        {
            if( ui->tabWidget->tabText(index) == prev_ID)
            {
                ui->tabWidget->setTabText(index, new_ID);
                _tab_info.insert( {new_ID, _tab_info.at(prev_ID)}  );
                _tab_info.erase( prev_ID );
                break;
            }
        }
    });

    auto createSingleTabBehaviorTree = [this](const AbsBehaviorTree &tree, const QString &bt_name)
    {
      onCreateAbsBehaviorTree(tree, bt_name, false);
    };

    connect( _replay_widget, &SidepanelReplay::loadBehaviorTree,
            this, createSingleTabBehaviorTree);

    connect( _replay_widget, &SidepanelReplay::addNewModel,
            this, &MainWindow::onAddToModelRegistry);

    connect( ui->toolButtonSaveFile, &QToolButton::clicked,
            this, &MainWindow::on_actionSave_triggered );

    connect( ui->toolButtonSaveFileAs, &QToolButton::clicked,
             this, &MainWindow::on_actionSaveAs_triggered );

    connect( _replay_widget, &SidepanelReplay::changeNodeStyle,
            this, &MainWindow::onChangeNodesStatus);

#ifdef ZMQ_FOUND

    connect( _monitor_widget, &SidepanelMonitor::addNewModel,
            this, &MainWindow::onAddToModelRegistry);

    connect( _monitor_widget, &SidepanelMonitor::changeNodeStyle,
            this, &MainWindow::onChangeNodesStatus);

    connect( _monitor_widget, &SidepanelMonitor::loadBehaviorTree,
            this, createSingleTabBehaviorTree );
#endif

    ui->tabWidget->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect( ui->tabWidget->tabBar(), &QTabBar::customContextMenuRequested,
            this, &MainWindow::onTabCustomContextMenuRequested);

    createTab("BehaviorTree");
    onTabSetMainTree(0);
    onSceneChanged();
    _current_state = saveCurrentState();
}


void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings settings;

    settings.setValue("MainWindow/geometry", saveGeometry());
    settings.setValue("MainWindow/windowState", saveState());

    switch(_current_layout )
    {
    case QtNodes::PortLayout::Horizontal:  settings.setValue("MainWindow/layout", "HORIZONTAL");
        break;
    case QtNodes::PortLayout::Vertical:  settings.setValue("MainWindow/layout", "VERTICAL");
        break;
    }

    settings.setValue("StartupDialog.Mode", toStr( _current_mode ) );

    ensureTreeSaved();

    QMainWindow::closeEvent(event);
}


GraphicContainer* MainWindow::createTab(const QString &name)
{
    if( _tab_info.count(name) > 0)
    {
        throw std::runtime_error(std::string("There is already a Tab named ") + name.toStdString() );
    }
    GraphicContainer* ti = new GraphicContainer( _model_registry, this );
    _tab_info.insert( {name, ti } );

    ti->scene()->setLayout( _current_layout );

    ui->tabWidget->addTab( ti->view(), name );

    ti->scene()->createNodeAtPos( "Root", "Root", QPointF(-30,-30) );
    ti->zoomHomeView();

    //--------------------------------

    connect( ti, &GraphicContainer::undoableChange,
            this, &MainWindow::onPushUndo );

    connect( ti, &GraphicContainer::undoableChange,
            this, &MainWindow::onSceneChanged );

    connect( _editor_widget, &SidepanelEditor::paletteEdited, 
            this, &MainWindow::onSidePaletteChanged);

    connect( ti, &GraphicContainer::requestSubTreeExpand,
            this, &MainWindow::onRequestSubTreeExpand );

    connect( ti, &GraphicContainer::requestSubTreeCreate,
            this, [this](const AbsBehaviorTree &tree, const QString &bt_name)
    {
      onCreateAbsBehaviorTree(tree, bt_name, false);
    });

    connect( ti, &GraphicContainer::addNewModel,
            this, &MainWindow::onAddToModelRegistry);

    return ti;
}

MainWindow::~MainWindow()
{
    delete ui;
}


void MainWindow::tryLoadWorkspace(const QString& workspace_text, bool overwriteOldWorkspace = true) {
    //load workspace. will default to document if the workspace is invalid
    QDomDocument workspaceDoc;
    bool docNotEmpty = true;
    if(!documentFromText(workspace_text, &workspaceDoc)) {
        docNotEmpty = false;
        qDebug() << "No workspace detected. Loading without a workspace." << endl;
    }

    if(overwriteOldWorkspace) {
        _workspace_models.clear();
    }

    if(docNotEmpty) {
        auto workspace_root = workspaceDoc.documentElement();

        //read tree nodes model
        _workspace_models = ReadTreeNodesModel(workspace_root);
        for(const auto& workspaceModelPair: _workspace_models) {
            NodeModel workspaceModel = workspaceModelPair.second;
            if(!isInNodeModels(_treenode_models, workspaceModelPair.first)) {
                onAddToModelRegistry(workspaceModel);
            } else {
                //check that the ports match
                bool portsMatch = true;
                NodeModel treenodeModel = _treenode_models[workspaceModelPair.first];

                if(treenodeModel.ports.size() == workspaceModel.ports.size()) {
                    for(const auto& wsPortPair : treenodeModel.ports) {
                        if(treenodeModel.ports.find(wsPortPair.first) == treenodeModel.ports.end()) {
                            portsMatch = false;
                            break;
                        }
                    }
                } else {
                    portsMatch = false;
                }

                if(!portsMatch) {
                    int reply = QMessageBox::question(this, 
                        "Conflicting Ports", 
                        tr("Node %1's ports in the local model differ from that in the workspace. Overwrite the workspace model with the local one? (answer \"no\" to use the workspace model)").arg(treenodeModel.registration_ID),
                        QMessageBox::Yes,
                        QMessageBox::No);
                    
                    if(reply == QMessageBox::No) { //load the workspace model
                        onAddToModelRegistry(workspaceModel);
                    }
                }
            }
        }

        //read subtree definitions
        for (auto bt_root = workspace_root.firstChildElement("BehaviorTree");
             !bt_root.isNull();
             bt_root = bt_root.nextSiblingElement("BehaviorTree"))
        {
            auto tree = BuildTreeFromXML( bt_root, _treenode_models );

            if( bt_root.hasAttribute("ID") )
            {
                QString tree_name = bt_root.attribute("ID");
                onCreateAbsBehaviorTree(tree, tree_name);
            }
        }
    }
}


bool MainWindow::loadFromXML(const QString& xml_text, const QString& workspace_text)
{
    //create a representation of the document being loaded
    QDomDocument docToLoad;
    if(!documentFromText(xml_text, &docToLoad)) {
        return false;
    }

    //---------------
    bool error = false;
    QString err_message;
    auto saved_state = _current_state;
    auto prev_tree_model = _treenode_models;

    //load desired tree
    try {
        auto document_root = docToLoad.documentElement();

        if( document_root.hasAttribute("main_tree_to_execute"))
        {
            _main_tree = document_root.attribute("main_tree_to_execute");
        }

        auto custom_models = ReadTreeNodesModel( document_root );

        for( const auto& model: custom_models)
        {
            NodeModel node = model.second;
            onAddToModelRegistry( node );
        }

        onClearRequested(false);

        const QSignalBlocker blocker( currentTabInfo() );

        for (auto bt_root = document_root.firstChildElement("BehaviorTree");
             !bt_root.isNull();
             bt_root = bt_root.nextSiblingElement("BehaviorTree"))
        {
            auto tree = BuildTreeFromXML( bt_root, _treenode_models );
            QString tree_name("BehaviorTree");

            if( bt_root.hasAttribute("ID") )
            {
                tree_name = bt_root.attribute("ID");
                if( _main_tree.isEmpty() )  // valid when there is only one
                {
                    _main_tree = tree_name;
                }
            }
            onCreateAbsBehaviorTree(tree, tree_name);
        }

        if( !_main_tree.isEmpty() )
        {
            for (int i=0; i< ui->tabWidget->count(); i++)
            {
                if( ui->tabWidget->tabText( i ) == _main_tree)
                {
                    ui->tabWidget->tabBar()->moveTab(i, 0);
                    ui->tabWidget->setCurrentIndex(0);
                    ui->tabWidget->tabBar()->setTabIcon(0, QIcon(":/icons/svg/star.svg"));
                    break;
                }
            }
        }

        if( currentTabInfo() == nullptr)
        {
            createTab("BehaviorTree");
            _main_tree = "BehaviorTree";
        }
        else{
            currentTabInfo()->nodeReorder();
        }

        tryLoadWorkspace(workspace_text);

        _editor_widget->updateTreeView();

        auto models_to_remove = GetModelsToRemove(this, _treenode_models, _workspace_models, custom_models);

        for( QString model_name: models_to_remove )
        {
            onModelRemoveRequested(model_name);
        }
    }
    catch (std::exception& err) {
        error = true;
        err_message = err.what();
    }

    if( error )
    {
        _treenode_models = prev_tree_model;
        loadSavedStateFromJson( saved_state );
        qDebug() << "R: Undo size: " << _undo_stack.size() << " Redo size: " << _redo_stack.size();
        QMessageBox::warning(this, tr("Exception!"),
                             tr("It was not possible to parse the file. Error:\n\n%1"). arg( err_message ),
                             QMessageBox::Ok);
        
        return false;
    }
    
    onSceneChanged();
    onPushUndo();
    return true;
}


void MainWindow::on_actionLoad_triggered()
{
    QSettings settings;
    QString directory_path  = settings.value("MainWindow.lastLoadDirectory",
                                            QDir::homePath() ).toString();

    QString fileName = QFileDialog::getOpenFileName(this,
                                                    tr("Load BehaviorTree from file"), directory_path,
                                                    tr("BehaviorTree files (*.xml)"));
    
    
    QString xml_text = readFileToString(fileName);
    if(xml_text.isEmpty()) { //either nothing in file or something went wrong
        return;
    }
    
    directory_path = QFileInfo(fileName).absolutePath();
    settings.setValue("MainWindow.lastLoadDirectory", directory_path);
    settings.sync();

    //load workspace text from file if a workspace is present
    QString 
        work_path = workspace_path(directory_path),
        workspace_text = readFileToString(work_path);

    bool success = loadFromXML(xml_text, workspace_text);
    if(success) {
        _current_file_name = fileName;
        updateTreeInfo(true, fileName);
    }
}

QString MainWindow::saveDocToXML() const
{
    QDomDocument doc;

    const char* COMMENT_SEPARATOR = " ////////// ";

    QDomElement root = doc.createElement( "root" );
    doc.appendChild( root );

    if( _main_tree.isEmpty() == false)
    {
        root.setAttribute("main_tree_to_execute", _main_tree.toStdString().c_str());
    }

    //encode subtrees
    for (auto& it: _tab_info)
    {
        encodeSubtree((QString) it.first, &doc, root, (GraphicContainer*) it.second);
    }
    root.appendChild( doc.createComment(COMMENT_SEPARATOR) );

    QDomElement root_models = doc.createElement("TreeNodesModel");

    for(const auto& tree_it: _treenode_models)
    {
        const auto& ID    = tree_it.first;
        const auto& model = tree_it.second;

        if( BuiltinNodeModels().count(ID) != 0 )
        {
            continue;
        }

        QDomElement node = doc.createElement( QString::fromStdString(toStr(model.type)) );

        encodeNodeModel(model, ID, doc, &node);
        root_models.appendChild(node);
    }
    root.appendChild(root_models);
    root.appendChild( doc.createComment(COMMENT_SEPARATOR) );

    return xmlDocumentToString(doc);
}


QString MainWindow::saveWorkspaceToXML() const {
    QDomDocument doc;
    
    const char *COMMENT_SEPARATOR = " ////////// ";

    QDomElement root = doc.createElement("root");
    doc.appendChild(root);

    QDomElement models = doc.createElement("TreeNodesModel");
    for(const auto& it : _workspace_models) {
        QString ID = it.first;
        NodeModel model = it.second;

        if( BuiltinNodeModels().count(ID) != 0 )
        {
            continue;
        }

        if(model.type == NodeType::SUBTREE) {
            encodeSubtree(ID, &doc, root);
        } 

        QDomElement node = doc.createElement(QString::fromStdString(toStr(model.type)));
        encodeNodeModel(model, ID, doc, &node);
        models.appendChild(node);
    }

    root.appendChild(models);
    root.appendChild(doc.createComment(COMMENT_SEPARATOR));

    return xmlDocumentToString(doc);
}


QString MainWindow::xmlDocumentToString(const QDomDocument &document) const
{
  QString output_string;

  QXmlStreamWriter stream(&output_string);

  stream.setAutoFormatting(true);
  stream.setAutoFormattingIndent(4);

  stream.writeStartDocument();

  auto root_element = document.documentElement();

  stream.writeStartElement(root_element.tagName());

  streamElementAttributes(stream, root_element);

  auto next_node = root_element.firstChild();

  while ( !next_node.isNull() )
  {
    recursivelySaveNodeCanonically(stream, next_node);

    if ( stream.hasError() )
    {
        break;
    }
    next_node = next_node.nextSibling();
  }

  stream.writeEndElement();
  stream.writeEndDocument();

  return output_string;
}

void MainWindow::streamElementAttributes(QXmlStreamWriter &stream, const QDomElement &element) const
{
    if (element.hasAttributes())
    {
        QMap<QString, QString> attributes;
        const QDomNamedNodeMap attributes_map = element.attributes();

        for (int i = 0; i < attributes_map.count(); ++i)
        {
           auto attribute = attributes_map.item(i);
            attributes.insert(attribute.nodeName(), attribute.nodeValue());
        }

        auto i = attributes.constBegin();
        while (i != attributes.constEnd())
        {
            stream.writeAttribute(i.key(), i.value());
            ++i;
        }
    }
}

void MainWindow::recursivelySaveNodeCanonically(QXmlStreamWriter &stream, const QDomNode &parent_node) const
{
  if ( stream.hasError() )
  {
    return;
  }

  if ( parent_node.isElement() )
  {
    const QDomElement parent_element = parent_node.toElement();

    if ( !parent_element.isNull() )
    {
        stream.writeStartElement(parent_element.tagName());

        streamElementAttributes(stream, parent_element);

        if (parent_element.hasChildNodes())
        {
            auto child = parent_element.firstChild();
            while ( !child.isNull() )
            {
                recursivelySaveNodeCanonically(stream, child);
                child = child.nextSibling();
            }
        }

        stream.writeEndElement();
    }
  }
  else if (parent_node.isComment())
  {
    stream.writeComment(parent_node.nodeValue());
  }
  else if (parent_node.isText())
  {
    stream.writeCharacters(parent_node.nodeValue());
  }
}

void MainWindow::on_actionSave_triggered()
{
    saveCurrentTree(false);
}

void MainWindow::on_actionSaveAs_triggered() 
{
    saveCurrentTree(true);
}

void MainWindow::onAutoArrange()
{
    currentTabInfo()->nodeReorder();
}

void MainWindow::onSceneChanged()
{
    const bool valid_BT = currentTabInfo()->containsValidTree();

    ui->toolButtonLayout->setEnabled(valid_BT);
    ui->toolButtonReorder->setEnabled(valid_BT);
    ui->toolButtonReorder->setEnabled(valid_BT);

    updateTreeInfo(false, _current_file_name);

    ui->actionSave->setEnabled(valid_BT);
    QPixmap pix;

    if(valid_BT)
    {
        pix.load(":/icons/green-circle.png");
        ui->labelSemaphore->setToolTip("Valid Tree");
    }
    else{
        pix.load(":/icons/red-circle.png");
        ui->labelSemaphore->setToolTip("NOT a valid Tree");
    }
    ui->labelSemaphore->setPixmap(pix);
    ui->labelSemaphore->setScaledContents(true);

    lockEditing( _current_mode != GraphicMode::EDITOR );
}


void MainWindow::onSidePaletteChanged() {
    updateTreeInfo(false, _current_file_name);
}


GraphicContainer* MainWindow::currentTabInfo()
{
    int index = ui->tabWidget->currentIndex();
    QString tab_name = ui->tabWidget->tabText(index);
    return getTabByName(tab_name);
}

GraphicContainer *MainWindow::getTabByName(const QString &tab_name)
{
    auto it = _tab_info.find( tab_name );
    return (it != _tab_info.end()) ? (it->second) : nullptr;
}


void MainWindow::lockEditing(bool locked)
{
    for(auto& tab_it: _tab_info)
    {
        tab_it.second->lockEditing(locked);
    }
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    auto view = dynamic_cast<QtNodes::FlowView*>(obj);
    if( view )
    {
        qDebug() << "A " << event->type();
        return false;
    }
    else
    {
        qDebug() << "B " << event->type();
        return QMainWindow::eventFilter(obj,event);
    }
}

void MainWindow::resizeEvent(QResizeEvent *)
{
    on_splitter_splitterMoved();
}


void MainWindow::on_splitter_splitterMoved(int , int )
{
    this->update();
    QList<int> sizes = ui->splitter->sizes();
    const int maxLeftWidth = ui->leftFrame->maximumWidth();
    int totalWidth = sizes[0] + sizes[1];

    if( sizes[0] > maxLeftWidth)
    {
        sizes[0] = maxLeftWidth;
        sizes[1] = totalWidth - maxLeftWidth;
        ui->splitter->setSizes(sizes);
    }
}

MainWindow::SavedState MainWindow::saveCurrentState()
{
    SavedState saved;
    int index = ui->tabWidget->currentIndex();
    saved.main_tree = _main_tree;
    saved.current_tab_name = ui->tabWidget->tabText(index);
    auto current_view = getTabByName( saved.current_tab_name )->view();
    saved.view_transform = current_view->transform();
    saved.view_area = current_view->sceneRect();

    for (auto& it: _tab_info)
    {
        saved.json_states[it.first] = it.second->scene()->saveToMemory();
    }
    return saved;
}

void MainWindow::onPushUndo()
{
    SavedState saved = saveCurrentState();

    if( _undo_stack.empty() || ( saved != _current_state &&  _undo_stack.back() != _current_state) )
    {
        _undo_stack.push_back( std::move(_current_state) );
        _redo_stack.clear();
    }
    _current_state = saved;

    //qDebug() << "P: Undo size: " << _undo_stack.size() << " Redo size: " << _redo_stack.size();
}

void MainWindow::onUndoInvoked()
{
    if ( _current_mode != GraphicMode::EDITOR ) return; //locked

    if( _undo_stack.size() > 0)
    {
        _redo_stack.push_back( std::move(_current_state) );
        _current_state = _undo_stack.back();
        _undo_stack.pop_back();

        loadSavedStateFromJson(_current_state);

        // qDebug() << "U: Undo size: " << _undo_stack.size() << " Redo size: " << _redo_stack.size();
    }
}

void MainWindow::onRedoInvoked()
{
    if ( _current_mode != GraphicMode::EDITOR ) return; //locked

    if( _redo_stack.size() > 0)
    {
        _undo_stack.push_back( _current_state );
        _current_state = std::move( _redo_stack.back() );
        _redo_stack.pop_back();

        loadSavedStateFromJson(_current_state);

        // qDebug() << "R: Undo size: " << _undo_stack.size() << " Redo size: " << _redo_stack.size();
    }
}

void MainWindow::loadSavedStateFromJson(SavedState saved_state)
{
    // TODO crash if the name of the container (tab) changed
    for (auto& it: _tab_info)
    {
        it.second->clearScene();
        it.second->deleteLater();
    }
    _tab_info.clear();
    ui->tabWidget->clear();

    _main_tree = saved_state.main_tree;

    for(const auto& it: saved_state.json_states)
    {
        QString tab_name = it.first;
        _tab_info.insert( {tab_name, createTab(tab_name)} );
    }
    for(const auto& it: saved_state.json_states)
    {
        QString name = it.first;
        auto container = getTabByName(name);
        container->loadFromJson( it.second );
        container->view()->setTransform( saved_state.view_transform );
        container->view()->setSceneRect( saved_state.view_area );
    }

    for (int i=0; i< ui->tabWidget->count(); i++)
    {
        if( ui->tabWidget->tabText( i ) == saved_state.current_tab_name)
        {
            ui->tabWidget->setCurrentIndex(i);
            ui->tabWidget->widget(i)->setFocus();
        }
        if( ui->tabWidget->tabText(i) == _main_tree)
        {
            onTabSetMainTree(i);
        }
    }
    if( ui->tabWidget->count() == 1 )
    {
        onTabSetMainTree(0);
    }
    onSceneChanged();
}

void MainWindow::onConnectionUpdate(bool connected)
{
    if(connected)
    {
        ui->toolButtonConnect->setStyleSheet("background-color: rgb(50, 150, 0); color:white");
        ui->toolButtonConnect->setText("Disconnect");
    }
    else{
        ui->toolButtonConnect->setStyleSheet(
            "QToolButton {color:white; }"
            "QToolButton:hover{ background-color: rgb(110, 110, 110); }"
            "QToolButton:pressed{ background-color: rgb(50, 150, 0) }"
            "QToolButton:disabled{color:gray; background-color: rgb(50, 50, 50) }");
        ui->toolButtonConnect->setText("Connect");
    }
}

void MainWindow::onRequestSubTreeExpand(GraphicContainer& container,
                                        QtNodes::Node& node)
{
    auto subtree = dynamic_cast< SubtreeNodeModel*>( node.nodeDataModel() );

    if( !subtree )
    {
        throw std::logic_error("passing to onRequestSubTreeExpand something that is not a SubTree");
    }

    if( subtree->expanded() )
    {
        subTreeExpand( container, node, SUBTREE_COLLAPSE );
    }
    else
    {
        subTreeExpand( container, node, SUBTREE_EXPAND );
    }
}


void MainWindow::onAddToModelRegistry(const NodeModel &model)
{
    namespace util = QtNodes::detail;
    const auto& ID = model.registration_ID;

    DataModelRegistry::RegistryItemCreator node_creator = [model]() -> DataModelRegistry::RegistryItemPtr
    {
        if( model.type == NodeType::SUBTREE)
        {
            return util::make_unique<SubtreeNodeModel>(model);
        }
        return util::make_unique<BehaviorTreeDataModel>(model);
    };

    _model_registry->registerModel( QString::fromStdString( toStr(model.type)), node_creator, ID);

    if(_treenode_models.find(ID) != _treenode_models.end()) {
        _treenode_models.erase(ID);
    }
    
    _treenode_models.insert( {ID, model } );
    _editor_widget->updateTreeView();
}

void MainWindow::onDestroySubTree(const QString &ID)
{
    auto sub_container = getTabByName(ID);

    for(auto& it: _tab_info)
    {
        if( it.first == ID )
        {
            continue;
        }
        auto container = it.second;
        auto tree = BuildTreeFromScene(container->scene());
        for( const auto& abs_node: tree.nodes())
        {
            auto qt_node = abs_node.graphic_node;
            auto bt_node = dynamic_cast<BehaviorTreeDataModel*>(qt_node->nodeDataModel());
            if(bt_node->nodeType() == NodeType::SUBTREE && bt_node->instanceName() == ID)
            {
                auto new_node = qt_node;
                auto subtree_model = dynamic_cast<SubtreeNodeModel*>(bt_node);
                if( subtree_model && subtree_model->expanded() == false )
                {
                    new_node = subTreeExpand( *container, *qt_node,
                                             SubtreeExpandOption::SUBTREE_EXPAND );
                }
                container->lockSubtreeEditing(*new_node, false, false);
                container->onSmartRemove( new_node );
            }
        }
        container->nodeReorder();
    }

    for( int index = 0; index < ui->tabWidget->count(); index++)
    {
        if( ui->tabWidget->tabText(index) == ID)
        {
            sub_container->scene()->clearScene();
            sub_container->deleteLater();
            ui->tabWidget->removeTab( index );
            _tab_info.erase(ID);
            break;
        }
    }

    if( ui->tabWidget->count() == 1 )
    {
        onTabSetMainTree(0);
    }

    // TODO: this is a work around until we find a better solution
    clearUndoStacks();
}

void MainWindow::onModelRemoveRequested(QString ID)
{
    BehaviorTreeDataModel* node_found = nullptr;
    QString tab_containing_node;

    for (auto& it: _tab_info)
    {
        auto container = it.second;
        for(const auto& node_it: container->scene()->nodes() )
        {
            QtNodes::Node* graphic_node = node_it.second.get();
            auto bt_node = dynamic_cast<BehaviorTreeDataModel*>( graphic_node->nodeDataModel() );

            if( bt_node->model().registration_ID == ID )
            {
                node_found = bt_node;
                tab_containing_node = it.first;
                break;
            }
        }
        if( node_found != nullptr )
        {
            break;
        }
    }

    if( !node_found )
    {
        _editor_widget->onRemoveModel(ID);
        return;
    }

    NodeType node_type = _treenode_models.at(ID).type;

    if( node_found && node_type != NodeType::SUBTREE )
    {
        QMessageBox::warning(this, "Can't remove this Model",
                             QString( "You are using this model in the Tree called [%1].\n"
                                     "You can't delete this model unless you "
                                     "remove all the instances of [%2].").arg(tab_containing_node, ID ),
                             QMessageBox::Ok );
    }
    else
    {
        int ret = QMessageBox::Cancel;
        if( node_found->model().type != NodeType::SUBTREE )
        {
            ret = QMessageBox::warning(this,"Delete TreeNode Model?",
                                       "Are you sure? This action can't be undone.",
                                       QMessageBox::Cancel | QMessageBox::Yes,
                                       QMessageBox::Cancel);
        }
        else{
            ret = QMessageBox::warning(this,"Delete Subtree?",
                                       "The Model of the Subtrees will be removed."
                                       "An expanded version will be added to parent trees.\n"
                                       "Are you sure? This action can't be undone.",
                                       QMessageBox::Cancel | QMessageBox::Yes,
                                       QMessageBox::Cancel);
        }

        if(ret == QMessageBox::Yes )
        {
            _editor_widget->onRemoveModel(ID);
            clearUndoStacks();
        }
    }
}

QtNodes::Node* MainWindow::subTreeExpand(GraphicContainer &container,
                                         QtNodes::Node &node,
                                         MainWindow::SubtreeExpandOption option)
{
    bool is_editor_mode = (_current_mode == GraphicMode::EDITOR);
    const QSignalBlocker blocker( this );
    auto subtree_model = dynamic_cast<SubtreeNodeModel*>(node.nodeDataModel());
    const QString& subtree_name = subtree_model->registrationName();

    if( option == SUBTREE_EXPAND && subtree_model->expanded() == false)
    {
        auto subtree_container = getTabByName(subtree_name);

        // Prevent expansion of invalid subtree
        if( !subtree_container->containsValidTree() )
        {
            QMessageBox::warning(this, tr("Oops!"),
                                 tr("Invalid SubTree. Can not expand SubTree."),
                                 QMessageBox::Cancel);
            return &node;
        }

        auto abs_subtree = BuildTreeFromScene( subtree_container->scene() );

        subtree_model->setExpanded(true);
        node.nodeState().getEntries(PortType::Out).resize(1);
        container.appendTreeToNode( node, abs_subtree );
        container.lockSubtreeEditing( node, true, is_editor_mode );

        if( abs_subtree.nodes().size() > 1 )
        {
            container.nodeReorder();
        }

        return &node;
    }

    if( option == SUBTREE_COLLAPSE && subtree_model->expanded() == true)
    {
        bool need_reorder = true;
        const auto& conn_out = node.nodeState().connections(PortType::Out, 0 );
        QtNodes::Node* child_node = nullptr;
        if(conn_out.size() == 1)
        {
            child_node = conn_out.begin()->second->getNode( PortType::In );
        }

        const QSignalBlocker blocker( container );
        if( child_node)
        {
            container.deleteSubTreeRecursively( *child_node );
        }
        else{
            need_reorder = false;
        }

        subtree_model->setExpanded(false);
        node.nodeState().getEntries(PortType::Out).resize(0);
        container.lockSubtreeEditing( node, false, is_editor_mode );
        if( need_reorder )
        {
            container.nodeReorder();
        }

        return &node;
    }

    if( option == SUBTREE_REFRESH && subtree_model->expanded() == true )
    {
        const auto& conn_out = node.nodeState().connections(PortType::Out, 0 );
        if(conn_out.size() != 1)
        {
            throw std::logic_error("subTreeExpand with SUBTREE_REFRESH, but not an expanded SubTree");
        }

        QtNodes::Node* child_node = conn_out.begin()->second->getNode( PortType::In );

        auto subtree_container = getTabByName(subtree_name);
        auto subtree = BuildTreeFromScene( subtree_container->scene() );

        container.deleteSubTreeRecursively( *child_node );
        container.appendTreeToNode( node, subtree );
        container.nodeReorder();
        container.lockSubtreeEditing( node, true, is_editor_mode );

        return &node;
    }

    return nullptr;
}

void MainWindow::on_toolButtonReorder_pressed()
{
    onAutoArrange();
}

void MainWindow::on_toolButtonCenterView_pressed()
{
    currentTabInfo()->zoomHomeView();
}

void MainWindow::clearUndoStacks()
{
    _undo_stack.clear();
    _redo_stack.clear();
    onSceneChanged();
    onPushUndo();
}

void MainWindow::onCreateAbsBehaviorTree(const AbsBehaviorTree &tree,
                                         const QString &bt_name,
                                         bool secondary_tabs)
{
    auto container = getTabByName(bt_name);
    if( !container )
    {
        container = createTab(bt_name);
    }
    const QSignalBlocker blocker( container );
    container->loadSceneFromTree( tree );
    container->nodeReorder();

    if( secondary_tabs ) {
      for(const auto& node: tree.nodes())
      {
        if( node.model.type == NodeType::SUBTREE && getTabByName(node.model.registration_ID) == nullptr)
        {
          createTab(node.model.registration_ID);
        }
      }
    }

    clearUndoStacks();
}

void MainWindow::on_actionNew_triggered()
{
    onClearRequested(true);
    clearTreeModels();
    clearUndoStacks();
    updateTreeInfo(false, "");
}

void MainWindow::onTreeNodeEdited(QString prev_ID, QString new_ID)
{
    //update workspace models. need to do it in this function rather than onAddToModelRegistry because this function has previous and new id
    bool
        oldIdInWorkspace = isInNodeModels(_workspace_models, prev_ID),
        newIdInWorkspace = isInNodeModels(_workspace_models, new_ID);

    if(newIdInWorkspace) {
        _workspace_models.erase(new_ID);
    }

    //what if the node was renamed?
    if(oldIdInWorkspace) {
        _workspace_models.erase(prev_ID);
    }

    if(oldIdInWorkspace || newIdInWorkspace) {
        //add model with new id to workspace
        _workspace_models.insert({new_ID, _treenode_models[new_ID]});
    }

    for (auto& it: _tab_info)
    {
        auto container = it.second;
        std::vector<QtNodes::Node*> nodes_to_rename;

        for(const auto& node_it: container->scene()->nodes() )
        {
            QtNodes::Node* graphic_node = node_it.second.get();
            if( !graphic_node )  {
                continue;
            }
            auto bt_node = dynamic_cast<BehaviorTreeDataModel*>( graphic_node->nodeDataModel() );
            if( !bt_node ) {
                continue;
            }

            if( bt_node->model().registration_ID == prev_ID )
            {
                nodes_to_rename.push_back( graphic_node );
            }
        }

        for(auto& graphic_node: nodes_to_rename )
        {
            auto bt_node = dynamic_cast<BehaviorTreeDataModel*>( graphic_node->nodeDataModel() );
            bool is_expanded_subtree = false;

            if( bt_node->model().type == NodeType::SUBTREE)
            {
                auto subtree_model = dynamic_cast<SubtreeNodeModel*>( bt_node );
                if(subtree_model && subtree_model->expanded())
                {
                    is_expanded_subtree = true;
                    subTreeExpand( *container, *graphic_node, SUBTREE_COLLAPSE);
                }
            }

            auto new_node = container->substituteNode( graphic_node, new_ID);

            if( is_expanded_subtree )
            {
                subTreeExpand( *container, *new_node, SUBTREE_EXPAND);
            };
        }
    }
}


void MainWindow::onClearRequested(bool create_new)
{
    for (auto& it: _tab_info)
    {
        it.second->clearScene();
        it.second->deleteLater();
    }
    _tab_info.clear();

    ui->tabWidget->clear();
    if( create_new )
    {
        createTab("BehaviorTree");
    }

    _editor_widget->clear();
    _replay_widget->clear();
#ifdef ZMQ_FOUND
    _monitor_widget->clear();
#endif

}

//populates out as a document representation from text
bool MainWindow::documentFromText(QString text, QDomDocument *out) {
    if(text.isEmpty()) {
        return false;
    }

    QDomDocument document;
    try{
        QString errorMsg;
        int errorLine;
        if( ! document.setContent(text, &errorMsg, &errorLine ) )
        {
            throw std::runtime_error( tr("Error parsing XML (line %1): %2").arg(errorLine).arg(errorMsg).toStdString() );
        }
        //---------------
        std::vector<QString> registered_ID;
        for (const auto& it: _treenode_models)
        {
            registered_ID.push_back( it.first );
        }
        // std::vector<QString> error_messages;
        // bool done = VerifyXML(document, registered_ID, error_messages );

        // if( !done )
        // {
        //     QString merged_error;
        //     for (const auto& err: error_messages)
        //     {
        //         merged_error += err + "\n";
        //     }
        //     throw std::runtime_error( merged_error.toStdString() );
        // }
    }
    catch( std::runtime_error& err)
    {
        QMessageBox messageBox;
        messageBox.critical(this,"Error parsing the XML", err.what() );
        messageBox.show();
        return false;
    }

    *out = document;
    return true;
}

std::vector<MainWindow::InvalidPortMapping> MainWindow::checkRequiredPorts() {
    std::vector<MainWindow::InvalidPortMapping> invalid_mappings;

    // Iterate through each sub tree
    for (auto i : _tab_info) {
        // Get behavior tree
        AbsBehaviorTree tree = BuildTreeFromScene(i.second->scene());

        // Iterator through each node in the tree
        for (AbstractTreeNode node : tree.nodes()) {

            // Get the PortModel
            PortModels port_models = node.model.ports;
            PortsMapping ports_mapping = node.ports_mapping;

            // Iterate through each port_model. If the port is required, check to make sure
            // the corresponding port_mapping is filled
            for (auto mapping : ports_mapping) {
                QString key = mapping.first;
                auto port = port_models[key];

                if (port.required) {
                    // Get value from port_mapping
                    QString value = mapping.second;
                    if (value == "") {
                        // Add invalid port mapping to be displayed to the screen
                        MainWindow::InvalidPortMapping mapping = {  .sub_tree = i.first, 
                                                        .node_id = node.model.registration_ID,
                                                        .port = key};
                        
                        invalid_mappings.push_back(mapping);
                    }
                }
            }
        }
    }

    return invalid_mappings;
}

void MainWindow::saveCurrentTree(bool forceSaveAs) {
    for (auto& it: _tab_info)
    {
        QString name = it.first; 
        auto& container = it.second;
        if( !container->containsValidTree() )
        {
            QMessageBox::warning(this, tr("Oops!"),
                                 tr("Tree \"%1\" is invalid. File can not be saved").arg(name),
                                 QMessageBox::Cancel);
            return;
        }
    }

    if( _tab_info.size() == 1 )
    {
        _main_tree = _tab_info.begin()->first;
    }

    std::vector<MainWindow::InvalidPortMapping> invalid_mappings = checkRequiredPorts();

    if (invalid_mappings.size() > 0) {
        QString message = "The following ports are marked 'required' yet they have no value assigned to them:\n\n";

        for (auto mapping : invalid_mappings) {
            message += "- Port '" + mapping.port + "' in node '" + mapping.node_id + "' in tree '" + mapping.sub_tree + "'\n\n";
        }

        QMessageBox::warning(this, tr("Oops!"),
                                    message);
        return;
    }

    QSettings settings;
    QString directory_path  = settings.value("MainWindow.lastSaveDirectory",
                                            QDir::currentPath() ).toString();

    QString fileName = _current_file_name;
    bool save_as = fileName.isEmpty() || forceSaveAs;
    if(save_as) {
        fileName = QFileDialog::getSaveFileName(this, "Save BehaviorTree to file",
                                                 directory_path, "BehaviorTree files (*.xml)");
    }

    if (fileName.isEmpty()){
        return;
    }
    if (!fileName.endsWith(".xml"))
    {
        fileName += ".xml";
    }

    //save current tree
    QString xml_text = saveDocToXML();

    QFile file(fileName);
    if (file.open(QIODevice::WriteOnly)) {
        QTextStream stream(&file);
        stream << xml_text << endl;
        file.close();
    }

    directory_path = QFileInfo(fileName).absolutePath();

    //save workspace
    QString work_dir = workspace_dir(directory_path);

    if(save_as) {
        //if saving as, overwrite current workspace with new one, then prompt user
        NodeModels old_workspace(_workspace_models);
        if(QDir(work_dir).exists()) {
            tryLoadWorkspace(readFileToString(workspace_path(directory_path)));
        }

        //figure out which nodes are not already in new workspace
        NodeModels unworkspacedModels;
        for(auto& it : old_workspace) {
            if(!isInNodeModels(_workspace_models, it.first)) {
                unworkspacedModels.insert(it);
            }
        }

        if(unworkspacedModels.size() > 0) {
            QString msg = "The following node models were in the previous workspace but not in the new one:\n";
            int count = 0;
            for(auto& it : unworkspacedModels) {
                msg += it.first + "\n";
                count++;

                if(count >= 3) {
                    msg += tr("...and %1 more\n").arg(unworkspacedModels.size() - 3);
                    break;
                }
            }

            msg += "Would you like to add them to the new workspace?";
            int ret = QMessageBox::question(this, "Add nodes to workspace?", msg, QMessageBox::Yes | QMessageBox::No);

            if(ret == QMessageBox::Yes) {
                for(auto& it : unworkspacedModels) {
                    _workspace_models.insert(it);
                }

                _editor_widget->updateTreeView();
            }
        }
    }

    QString workspace_text = saveWorkspaceToXML(); //encode workspace as xml text

    if(!QDir(work_dir).exists()) {
        QDir().mkdir(work_dir);
    }

    QFile workspaceFile(workspace_path(directory_path));
    if (workspaceFile.open(QIODevice::WriteOnly)) {
        QTextStream stream(&workspaceFile);
        stream << workspace_text << endl;
        workspaceFile.close();
    }

    updateTreeInfo(true, fileName);
    settings.setValue("MainWindow.lastSaveDirectory", directory_path);
}


void MainWindow::encodeSubtree(QString ID, QDomDocument *doc, QDomElement root, GraphicContainer *container) const {
    auto scene = container->scene();

    auto abs_tree = BuildTreeFromScene(container->scene());
    auto abs_root = abs_tree.rootNode();
    if( abs_root->children_index.size() == 1 &&
        abs_root->model.registration_ID == "Root"  )
    {
        // mofe to the child of ROOT
        abs_root = abs_tree.node( abs_root->children_index.front() );
    }

    QtNodes::Node* root_node = abs_root->graphic_node;

    QDomElement root_element = doc->createElement("BehaviorTree");

    root_element.setAttribute("ID", ID.toStdString().c_str());
    root.appendChild(root_element);

    RecursivelyCreateXml(*scene, *doc, root_element, root_node );
}


void MainWindow::encodeSubtree(QString ID, QDomDocument *doc, QDomElement root) const {
    //encode subtrees
    GraphicContainer* container = nullptr;
    
    //find the container containing the subtree to be encoded
    for (auto& it: _tab_info) {
        if(it.first == ID) {
            container = it.second;
            break;
        }
    }

    if(container == nullptr) { //container was never found
        return;
    }

    //encode subtree with found container
    encodeSubtree(ID, doc, root, container);
}

void MainWindow::encodeNodeModel(NodeModel model, QString id, QDomDocument doc, QDomElement *node) const {
    if( !node->isNull() )
    {
        node->setAttribute("ID", id);

        for(const auto& port_it: model.ports)
        {
            const auto& port_name = port_it.first;
            const auto& port = port_it.second;

            QDomElement port_element = writePortModel(port_name, port, doc);
            node->appendChild( port_element );
        }
    }
}

//use saved for current save status and _current_file_name for current file name 
void MainWindow::updateTreeInfo(bool saved, QString fileName) {
    //which icon to use on save button?
    this->saved = saved;
    QString icoFile = (saved ? ":/icons/svg/saved_white.svg" : ":/icons/svg/unsaved_white.svg");
    ui->toolButtonSaveFile->setIcon(QIcon(icoFile));

    _current_file_name = fileName;
    QString fileStr = (!fileName.isEmpty() ? fileName : "No File loaded");
    QString asterisk = (saved || fileName.isEmpty() ? "" : "*");

    fileStr = tr("%1%2").arg(fileStr, asterisk); //add asterisk if file is unsaved
    this->setWindowTitle(tr("Groot (%1)").arg(fileStr));
}


void MainWindow::ensureTreeSaved() {
    if(_current_mode == GraphicMode::EDITOR && !saved) {
        int save = QMessageBox::question(this, "Save Tree?", 
                                        "You have unsaved changes to your Behavior Tree. Do you want to save them?", 
                                        QMessageBox::No | QMessageBox::Yes);
        
        if(save == QMessageBox::Yes) {
            saveCurrentTree(false);
        }
    }
}


void MainWindow::updateCurrentMode()
{
    const bool NOT_EDITOR = _current_mode != GraphicMode::EDITOR;

    _editor_widget->setHidden( NOT_EDITOR );
    _replay_widget->setHidden( _current_mode != GraphicMode::REPLAY );
#ifdef ZMQ_FOUND
    _monitor_widget->setHidden( _current_mode != GraphicMode::MONITOR );
#endif

    ui->toolButtonLoadFile->setHidden( _current_mode == GraphicMode::MONITOR );
    ui->toolButtonConnect->setHidden( _current_mode != GraphicMode::MONITOR );

    if( _current_mode == GraphicMode::EDITOR )
    {
        ui->toolButtonLoadFile->setText("Load Tree");
    }
    else if( _current_mode == GraphicMode::REPLAY )
    {
        ui->toolButtonLoadFile->setText("Load Log");
    }

    ui->toolButtonLoadRemote->setHidden( true );

    ui->toolButtonSaveFile->setHidden( NOT_EDITOR );
    ui->toolButtonSaveFileAs->setHidden( NOT_EDITOR );
    ui->toolButtonReorder->setHidden( NOT_EDITOR );

    if( _current_mode == GraphicMode::EDITOR )
    {
        connect( ui->toolButtonLoadFile, &QToolButton::clicked,
                this, &MainWindow::on_actionLoad_triggered );
        disconnect( ui->toolButtonLoadFile, &QToolButton::clicked,
                   _replay_widget, &SidepanelReplay::on_LoadLog );
    }
    else if( _current_mode == GraphicMode::REPLAY )
    {
        disconnect( ui->toolButtonLoadFile, &QToolButton::clicked,
                   this, &MainWindow::on_actionLoad_triggered );
        connect( ui->toolButtonLoadFile, &QToolButton::clicked,
                _replay_widget, &SidepanelReplay::on_LoadLog );
    }
    lockEditing( NOT_EDITOR );

    if( _current_mode == GraphicMode::EDITOR)
    {
        _editor_widget->updateTreeView();
    }
    ui->actionEditor_mode->setEnabled( _current_mode != GraphicMode::EDITOR);
#ifdef ZMQ_FOUND
    ui->actionMonitor_mode->setEnabled( _current_mode != GraphicMode::MONITOR);
#endif
    ui->actionReplay_mode->setEnabled( _current_mode != GraphicMode::REPLAY);
}


void MainWindow::refreshNodesLayout(QtNodes::PortLayout new_layout)
{
    if( new_layout != _current_layout)
    {
        QString icon_name = ( new_layout == QtNodes::PortLayout::Horizontal ) ?
                                                                            ":/icons/BT-horizontal.png" :
                                                                            ":/icons/BT-vertical.png";
        QIcon icon;
        icon.addFile(icon_name, QSize(), QIcon::Normal, QIcon::Off);
        ui->toolButtonLayout->setIcon(icon);
        ui->toolButtonLayout->update();
    }

    bool refreshed = false;
    {
        const QSignalBlocker blocker( currentTabInfo() );
        for(auto& tab: _tab_info)
        {
            auto scene = tab.second->scene();
            if( scene->layout() != new_layout )
            {
                auto abstract_tree = BuildTreeFromScene( scene );
                scene->setLayout( new_layout );
                NodeReorder( *scene, abstract_tree );
                refreshed = true;
            }
        }
        on_toolButtonCenterView_pressed();
    }
    _current_layout = new_layout;
    if(refreshed)
    {
        onPushUndo();
    }
}

void MainWindow::refreshExpandedSubtrees()
{
    auto container = currentTabInfo();
    if( !container){
        return;
    }
    auto scene = container->scene();
    auto root_node = findRoot( *scene );
    if( !root_node )
    {
        return;
    }

    std::vector<QtNodes::Node*> subtree_nodes;
    std::function<void(QtNodes::Node*)> selectRecursively;

    selectRecursively = [&](QtNodes::Node* node)
    {
        auto subtree_model = dynamic_cast<SubtreeNodeModel*>(node->nodeDataModel());
        if(subtree_model && subtree_model->expanded())
        {
            subtree_nodes.push_back( node );
        }
        else{
            auto children = getChildren( scene, *node, false );
            for(auto child_node: children)
            {
                selectRecursively(child_node);
            }
        }
    };
    selectRecursively( root_node );

    for (auto subtree_node: subtree_nodes)
    {
        // expanded subtrees may have become invalid
        // collapse invalid subtrees before refreshing them
        auto subtree_model = dynamic_cast<SubtreeNodeModel*>(subtree_node->nodeDataModel());
        const QString& subtree_name = subtree_model->registrationName();
        auto subtree_container = getTabByName(subtree_name);
        if ( subtree_model->expanded() && !subtree_container->containsValidTree() )
        {
            subTreeExpand( *container, *subtree_node, SUBTREE_COLLAPSE );
        }

        subTreeExpand( *container, *subtree_node, SUBTREE_REFRESH );
    }
}

void MainWindow::on_toolButtonLayout_clicked()
{
    if( _current_layout == QtNodes::PortLayout::Horizontal)
    {
        refreshNodesLayout( QtNodes::PortLayout::Vertical );
    }
    else{
        refreshNodesLayout( QtNodes::PortLayout::Horizontal );
    }
}

void MainWindow::on_actionEditor_mode_triggered()
{
    _current_mode = GraphicMode::EDITOR;
    updateCurrentMode();

#ifdef ZMQ_FOUND
    _monitor_widget->clear();
#endif

    _replay_widget->clear();
}

void MainWindow::on_actionMonitor_mode_triggered()
{
#ifdef ZMQ_FOUND
    if(_current_mode == GraphicMode::EDITOR) {
        ensureTreeSaved();
    }

    QMessageBox::StandardButton res = QMessageBox::Ok;

    if( currentTabInfo()->scene()->nodes().size() > 0)
    {
        res = QMessageBox::warning(this, tr("Carefull!"),
                                   tr("If you switch to Monitor Mode, "
                                      "the current BehaviorTree in the Scene will be deleted"),
                                   QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);
    }
    if( res == QMessageBox::Ok)
    {
        currentTabInfo()->clearScene();
        _monitor_widget->clear();
        _current_mode = GraphicMode::MONITOR;
        updateCurrentMode();
    }
#endif
}

void MainWindow::on_actionReplay_mode_triggered()
{
    if(_current_mode == GraphicMode::EDITOR) {
        ensureTreeSaved();
    }

    QMessageBox::StandardButton res = QMessageBox::Ok;

    if( currentTabInfo()->scene()->nodes().size() > 0)
    {
        res = QMessageBox::warning(this, tr("Carefull!"),
                                   tr("If you switch to Log Replay Mode, "
                                      "the current BehaviorTree in the Scene will be deleted"),
                                   QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);
    }
    if( res == QMessageBox::Ok)
    {
        onClearRequested(true);
        _replay_widget->clear();
        _current_mode = GraphicMode::REPLAY;
        updateCurrentMode();
    }
}

void MainWindow::on_tabWidget_currentChanged(int index)
{
    if( ui->tabWidget->count() == 0 )
    {
        return;
    }
    QString tab_name = ui->tabWidget->tabText(index);
    auto tab = getTabByName(tab_name);
    if( tab )
    {
        const QSignalBlocker blocker( tab );
        tab->nodeReorder();
        _current_state.current_tab_name = ui->tabWidget->tabText( index );
        refreshExpandedSubtrees();
        tab->zoomHomeView();
    }
}

bool MainWindow::SavedState::operator ==(const MainWindow::SavedState &other) const
{
    if( current_tab_name != other.current_tab_name ||
        json_states.size() != other.json_states.size())
    {
        return false;
    }
    for(auto& it: json_states  )
    {
        auto other_it = other.json_states.find(it.first);
        if( other_it == other.json_states.end() ||
            it.second != other_it->second)
        {
            return false;
        }
    }
    if( view_area != other.view_area ||
        view_transform != other.view_transform)
    {
        return false;
    }
    return true;
}

void MainWindow::resetTreeStyle(AbsBehaviorTree &tree){
    //printf("resetTreeStyle\n");
    QtNodes::NodeStyle  node_style;
    QtNodes::ConnectionStyle conn_style;

    for(auto abs_node: tree.nodes()){
        auto gui_node = abs_node.graphic_node;

        gui_node->nodeDataModel()->setNodeStyle( node_style );
        gui_node->nodeGraphicsObject().update();

        const auto& conn_in = gui_node->nodeState().connections(PortType::In, 0 );
        if(conn_in.size() == 1)
        {
            auto conn = conn_in.begin()->second;
            conn->setStyle( conn_style );
            conn->connectionGraphicsObject().update();
        }
    }
}

void MainWindow::onChangeNodesStatus(const QString& bt_name,
                                     const std::vector<std::pair<int, NodeStatus> > &node_status)
{
    auto tree = BuildTreeFromScene( getTabByName(bt_name)->scene() );

    std::vector<NodeStatus> vec_last_status(tree.nodesCount());

    // printf("---\n");

    for (auto& it: node_status)
    {
        const int index = it.first;
        const NodeStatus status = it.second;
        auto& abs_node = tree.nodes().at(index);

        // printf("%3d: %d, %s\n", index, (int)it.second, abs_node.instance_name.toStdString().c_str());

        if(index == 1 && it.second == NodeStatus::RUNNING)
            resetTreeStyle(tree);

        auto gui_node = abs_node.graphic_node;
        auto style = getStyleFromStatus( status, vec_last_status[index] );
        gui_node->nodeDataModel()->setNodeStyle( style.first );
        gui_node->nodeGraphicsObject().update();

        vec_last_status[index] = status;

        const auto& conn_in = gui_node->nodeState().connections(PortType::In, 0 );
        if(conn_in.size() == 1)
        {
            auto conn = conn_in.begin()->second;
            conn->setStyle( style.second );
            conn->connectionGraphicsObject().update();
        }
    }
}

void MainWindow::onTabCustomContextMenuRequested(const QPoint &pos)
{
    int tab_index = ui->tabWidget->tabBar()->tabAt( pos );

    QMenu menu(this);
    QAction* rename   = menu.addAction("Rename");

    connect( rename, &QAction::triggered, this, [this, tab_index]()
            {
                onTabRenameRequested(tab_index);
            } );

    QAction* set_main   = menu.addAction("Set as main tree");

    connect( set_main, &QAction::triggered, this, [this, tab_index]()
            {
                onTabSetMainTree(tab_index);
            } );

    QPoint globalPos = ui->tabWidget->tabBar()->mapToGlobal(pos);
    menu.exec(globalPos);
}

void MainWindow::onTabRenameRequested(int tab_index, QString new_name)
{
    QString old_name = this->ui->tabWidget->tabText(tab_index);

    if( new_name.isEmpty())
    {
        bool ok = false;
        new_name = QInputDialog::getText (
            this, tr ("Change name"),
            tr ("Insert the new name of this BehaviorTree"),
            QLineEdit::Normal, old_name, &ok);
        if (!ok)
        {
            return;
        }
    }

    if( new_name == old_name)
    {
        return;
    }
    if( getTabByName(new_name) )
    {
        QMessageBox::warning( this, "Tab name already is use",
                             tr("There is already a BehaviorTree called [%1].\n"
                                "Use another name.").arg(new_name),
                             QMessageBox::Ok);
        return;
    }

    ui->tabWidget->setTabText (tab_index, new_name);
    auto it = _tab_info.find(old_name);
    auto container = it->second;
    _tab_info.insert( {new_name, container} );
    _tab_info.erase( it );
    if( _main_tree == old_name )
    {
        _main_tree = new_name;
    }

    // if a subtree SUBTREE already
    if( _model_registry->registeredModelsByCategory("SubTree").count( old_name ) != 0 )
    {
        _model_registry->unregisterModel(old_name);
        _treenode_models.erase(old_name);
        NodeModel model = { NodeType::SUBTREE, new_name, {}};
        onAddToModelRegistry( model );
        _treenode_models.insert( { new_name, model} );
        _editor_widget->updateTreeView();
        this->onTreeNodeEdited(old_name, new_name);
    }

    // TODO: this is a work around until we find a better solution
    clearUndoStacks();
}


void MainWindow::onTabSetMainTree(int tab_index)
{
    for (int i=0; i<ui->tabWidget->count(); i++ )
    {
        if( i == tab_index )
        {
            ui->tabWidget->tabBar()->setTabIcon(i, QIcon(":/icons/svg/star.svg") );
            _main_tree = ui->tabWidget->tabBar()->tabText(i);
        }
        else{
            ui->tabWidget->tabBar()->setTabIcon( i, QIcon());
        }
    }
}


void MainWindow::clearTreeModels()
{
    _workspace_models.clear();
    _treenode_models = BuiltinNodeModels();

    std::list<QString> ID_to_delete;
    for(const auto& it: _model_registry->registeredModelCreators() )
    {
        const auto& ID = it.first;
        if( _treenode_models.count( ID ) == 0)
        {
            ID_to_delete.push_back(ID);
        }
    }
    for(const auto& ID: ID_to_delete )
    {
        _model_registry->unregisterModel(ID);
    }
    _editor_widget->updateTreeView();
}

const NodeModels &MainWindow::registeredModels() const
{
    return _treenode_models;
}

void MainWindow::on_actionAbout_triggered()
{
    auto ui = new Ui_Dialog;
    QDialog* dialog = new QDialog(this);
    ui->setupUi(dialog);

    auto svg_widget = new QSvgWidget( tr(":/icons/svg/logo_splashscreen.svg") );
    ui->frame->layout()->addWidget(svg_widget);
    dialog->setWindowFlags( Qt::SplashScreen );
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    dialog->show();
}

void MainWindow::on_actionReportIssue_triggered()
{
    const char* url = "https://github.com/BehaviorTree/Groot/issues";
    QMessageBox::warning( this, "Issue Reporting",
                         tr("Reporting an issue you allow us to make this software better and better. Thanks!\n"
                            "You will be redirected to our Github Page:\n\n"
                            "%1").arg(url),
                         QMessageBox::Ok);
    QDesktopServices::openUrl(QUrl(url));
}

// returns the current graphic mode
GraphicMode MainWindow::getGraphicMode(void) const
{
    return _current_mode;
}
