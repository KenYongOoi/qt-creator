/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "projectnodes.h"

#include "nodesvisitor.h"
#include "projectexplorerconstants.h"
#include "projecttree.h"

#include <coreplugin/fileiconprovider.h>
#include <coreplugin/icore.h>
#include <coreplugin/iversioncontrol.h>
#include <coreplugin/vcsmanager.h>
#include <utils/algorithm.h>
#include <utils/fileutils.h>
#include <utils/qtcassert.h>

#include <QFileInfo>
#include <QDir>
#include <QIcon>
#include <QStyle>

namespace ProjectExplorer {

/*!
  \class ProjectExplorer::Node

  \brief The Node class is the base class of all nodes in the node hierarchy.

  The nodes are arranged in a tree where leaves are FileNodes and non-leaves are FolderNodes
  A Project is a special Folder that manages the files and normal folders underneath it.

  The Watcher emits signals for structural changes in the hierarchy.
  A Visitor can be used to traverse all Projects and other Folders.

  \sa ProjectExplorer::FileNode, ProjectExplorer::FolderNode, ProjectExplorer::ProjectNode
  \sa ProjectExplorer::NodesWatcher, ProjectExplorer::NodesVisitor
*/

Node::Node(NodeType nodeType, const Utils::FileName &filePath, int line) :
    m_filePath(filePath), m_line(line), m_nodeType(nodeType)
{ }

void Node::setPriority(int p)
{
    m_priority = p;
}

void Node::setAbsoluteFilePathAndLine(const Utils::FileName &path, int line)
{
    if (m_filePath == path && m_line == line)
        return;

    m_filePath = path;
    m_line = line;
    emitNodeUpdated();
}

Node::~Node()
{

}

NodeType Node::nodeType() const
{
    return m_nodeType;
}

int Node::priority() const
{
    return m_priority;
}

/*!
  The project that owns and manages the node. It is the first project in the list
  of ancestors.
  */
ProjectNode *Node::parentProjectNode() const
{
    if (!m_parentFolderNode)
        return nullptr;
    auto pn = m_parentFolderNode->asProjectNode();
    if (pn)
        return pn;
    return m_parentFolderNode->parentProjectNode();
}

/*!
  The parent in the node hierarchy.
  */
FolderNode *Node::parentFolderNode() const
{
    return m_parentFolderNode;
}

ProjectNode *Node::managingProject()
{
    if (asSessionNode())
        return nullptr;
    ProjectNode *pn = parentProjectNode();
    return pn ? pn : asProjectNode(); // projects manage themselves...
}

const ProjectNode *Node::managingProject() const
{
    return const_cast<Node *>(this)->managingProject();
}

/*!
  The path of the file or folder in the filesystem the node represents.
  */
const Utils::FileName &Node::filePath() const
{
    return m_filePath;
}

int Node::line() const
{
    return m_line;
}

QString Node::displayName() const
{
    return filePath().fileName();
}

QString Node::tooltip() const
{
    return filePath().toUserOutput();
}

bool Node::isEnabled() const
{
    if (!m_isEnabled)
        return false;
    FolderNode *parent = parentFolderNode();
    return parent ? parent->isEnabled() : true;
}

QList<ProjectAction> Node::supportedActions(Node *node) const
{
    QList<ProjectAction> list = parentFolderNode()->supportedActions(node);
    list.append(InheritedFromParent);
    return list;
}

void Node::setEnabled(bool enabled)
{
    if (m_isEnabled == enabled)
        return;
    m_isEnabled = enabled;
    emitNodeUpdated();
}

void Node::emitNodeUpdated()
{
    if (parentFolderNode())
        ProjectTree::emitNodeUpdated(this);
}

void Node::emitTreeChanged()
{
    ProjectTree::emitDataChanged();
}

bool Node::sortByPath(const Node *a, const Node *b)
{
    return a->filePath() < b->filePath();
}

void Node::setParentFolderNode(FolderNode *parentFolder)
{
    m_parentFolderNode = parentFolder;
}

/*!
  \class ProjectExplorer::FileNode

  \brief The FileNode class is an in-memory presentation of a file.

  All file nodes are leaf nodes.

  \sa ProjectExplorer::FolderNode, ProjectExplorer::ProjectNode
*/

FileNode::FileNode(const Utils::FileName &filePath,
                   const FileType fileType,
                   bool generated, int line) : Node(NodeType::File, filePath, line),
    m_fileType(fileType),
    m_generated(generated)
{
    if (fileType == FileType::Project)
        setPriority(DefaultProjectFilePriority);
    else
        setPriority(DefaultFilePriority);
}

FileType FileNode::fileType() const
{
    return m_fileType;
}

/*!
  Returns \c true if the file is automatically generated by a compile step.
  */
bool FileNode::isGenerated() const
{
    return m_generated;
}

static QList<FileNode *> scanForFilesRecursively(const Utils::FileName &directory,
                                                 const std::function<FileNode *(const Utils::FileName &)> factory,
                                                 QSet<QString> &visited, QFutureInterface<QList<FileNode*>> *future,
                                                 double progressStart, double progressRange)
{
    QList<FileNode *> result;

    const QDir baseDir = QDir(directory.toString());

    // Do not follow directory loops:
    const int visitedCount = visited.count();
    visited.insert(baseDir.canonicalPath());
    if (visitedCount == visited.count())
        return result;

    const Core::IVersionControl *vcsControl
            = Core::VcsManager::findVersionControlForDirectory(baseDir.absolutePath(), nullptr);
    const QList<QFileInfo> entries = baseDir.entryInfoList(QStringList(), QDir::AllEntries|QDir::NoDotAndDotDot);
    double progress = 0;
    const double progressIncrement = progressRange / static_cast<double>(entries.count());
    int lastIntProgress = 0;
    for (const QFileInfo &entry : entries) {
        if (future && future->isCanceled())
            return result;

        const Utils::FileName entryName = Utils::FileName::fromString(entry.absoluteFilePath());
        if (!vcsControl || !vcsControl->isVcsFileOrDirectory(entryName)) {
            if (entry.isDir()) {
                result.append(scanForFilesRecursively(entryName, factory, visited, future, progress, progressIncrement));
            } else {
                FileNode *node = factory(entryName);
                if (node)
                    result.append(node);
            }
        }
        if (future) {
            progress += progressIncrement;
            const int intProgress = std::min(static_cast<int>(progressStart + progress), future->progressMaximum());
            if (lastIntProgress < intProgress) {
                future->setProgressValue(intProgress);
                lastIntProgress = intProgress;
            }
        }
    }
    if (future)
        future->setProgressValue(std::min(static_cast<int>(progressStart + progressRange), future->progressMaximum()));
    return result;
}

QList<FileNode *> FileNode::scanForFiles(const Utils::FileName &directory,
                                               const std::function<FileNode *(const Utils::FileName &)> factory,
                                               QFutureInterface<QList<FileNode *>> *future)
{
    QSet<QString> visited;
    if (future)
        future->setProgressRange(0, 1000000);
    return scanForFilesRecursively(directory, factory, visited, future, 0.0, 1000000.0);
}

/*!
  \class ProjectExplorer::FolderNode

  In-memory presentation of a folder. Note that the node itself + all children (files and folders) are "managed" by the owning project.

  \sa ProjectExplorer::FileNode, ProjectExplorer::ProjectNode
*/
FolderNode::FolderNode(const Utils::FileName &folderPath, NodeType nodeType, const QString &displayName) :
    Node(nodeType, folderPath, -1),
    m_displayName(displayName)
{
    setPriority(DefaultFolderPriority);
    if (m_displayName.isEmpty())
        m_displayName = folderPath.toUserOutput();
}

FolderNode::~FolderNode()
{
    qDeleteAll(m_folderNodes);
    qDeleteAll(m_fileNodes);
}

/*!
    Contains the display name that should be used in a view.
    \sa setFolderName()
 */

QString FolderNode::displayName() const
{
    return m_displayName;
}

/*!
  Contains the icon that should be used in a view. Default is the directory icon
 (QStyle::S_PDirIcon).
  s\a setIcon()
 */
QIcon FolderNode::icon() const
{
    // Instantiating the Icon provider is expensive.
    if (m_icon.isNull())
        m_icon = Core::FileIconProvider::icon(QFileIconProvider::Folder);
    return m_icon;
}

QList<FileNode*> FolderNode::fileNodes() const
{
    return m_fileNodes;
}

FileNode *FolderNode::fileNode(const Utils::FileName &file) const
{
    return Utils::findOrDefault(m_fileNodes, [&file](const FileNode *fn) {
        return fn->filePath() == file;
    });
}

FileNode *FolderNode::recursiveFileNode(const Utils::FileName &file) const
{
    Utils::FileName dir = file.parentDir();

    const QDir thisDir(filePath().toString());
    QString relativePath = thisDir.relativeFilePath(dir.toString());
    if (relativePath == ".")
        relativePath.clear();
    QStringList parts = relativePath.split('/', QString::SkipEmptyParts);
    const ProjectExplorer::FolderNode *parent = this;
    foreach (const QString &part, parts) {
        dir.appendPath(part);
        // Find folder in subFolders
        parent = Utils::findOrDefault(parent->folderNodes(), [&dir](const FolderNode *fn) {
            return fn->filePath() == dir;
        });
        if (!parent)
            return nullptr;
    }
    return parent->fileNode(file);
}

QList<FileNode *> FolderNode::recursiveFileNodes() const
{
    QList<FileNode *> result = fileNodes();
    foreach (ProjectExplorer::FolderNode *folder, folderNodes())
        result.append(folder->recursiveFileNodes());
    return result;
}

QList<FolderNode*> FolderNode::folderNodes() const
{
    return m_folderNodes;
}

FolderNode *FolderNode::folderNode(const Utils::FileName &directory) const
{
    return Utils::findOrDefault(m_folderNodes, [&directory](const FolderNode *fn) {
        return fn->filePath() == directory;
    });
}

FolderNode *FolderNode::recursiveFindOrCreateFolderNode(const QString &directory,
                                                        const Utils::FileName &overrideBaseDir)
{
    Utils::FileName path = overrideBaseDir.isEmpty() ? filePath() : overrideBaseDir;
    QString workPath;
    if (path.isEmpty() || path.toFileInfo().isRoot()) {
        workPath = directory;
    } else {
        QDir parentDir(path.toString());
        workPath = parentDir.relativeFilePath(directory);
        if (workPath == ".")
            workPath.clear();
    }
    const QStringList parts = workPath.split('/', QString::SkipEmptyParts);

    ProjectExplorer::FolderNode *parent = this;
    foreach (const QString &part, parts) {
        path.appendPath(part);
        // Find folder in subFolders
        FolderNode *next = parent->folderNode(path);
        if (!next) {
            // No FolderNode yet, so create it
            auto tmp = new ProjectExplorer::FolderNode(path);
            tmp->setDisplayName(part);
            parent->addFolderNode(tmp);
            next = tmp;
        }
        parent = next;
    }
    return parent;
}

void FolderNode::buildTree(QList<FileNode *> &files, const Utils::FileName &overrideBaseDir)
{
    makeEmpty();

    foreach (ProjectExplorer::FileNode *fn, files) {
        // Get relative path to rootNode
        QString parentDir = fn->filePath().toFileInfo().absolutePath();
        ProjectExplorer::FolderNode *folder = recursiveFindOrCreateFolderNode(parentDir, overrideBaseDir);
        folder->addFileNode(fn);
    }

    emitTreeChanged();
}

void FolderNode::accept(NodesVisitor *visitor)
{
    visitor->visitFolderNode(this);
    foreach (FolderNode *subFolder, m_folderNodes)
        subFolder->accept(visitor);
}

void FolderNode::setDisplayName(const QString &name)
{
    if (m_displayName == name)
        return;
    m_displayName = name;
    emitNodeUpdated();
}

void FolderNode::setIcon(const QIcon &icon)
{
    m_icon = icon;
}

QString FolderNode::addFileFilter() const
{
    return parentFolderNode()->addFileFilter();
}

bool FolderNode::addFiles(const QStringList &filePaths, QStringList *notAdded)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->addFiles(filePaths, notAdded);
    return false;
}

bool FolderNode::removeFiles(const QStringList &filePaths, QStringList *notRemoved)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->removeFiles(filePaths, notRemoved);
    return false;
}

bool FolderNode::deleteFiles(const QStringList &filePaths)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->deleteFiles(filePaths);
    return false;
}

bool FolderNode::canRenameFile(const QString &filePath, const QString &newFilePath)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->canRenameFile(filePath, newFilePath);
    return false;
}

bool FolderNode::renameFile(const QString &filePath, const QString &newFilePath)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->renameFile(filePath, newFilePath);
    return false;
}

FolderNode::AddNewInformation FolderNode::addNewInformation(const QStringList &files, Node *context) const
{
    Q_UNUSED(files);
    return AddNewInformation(displayName(), context == this ? 120 : 100);
}

/*!
  Adds a file node specified by \a file to the internal list of the folder
  and emits the corresponding signals from the projectNode.

  This function should be called within an implementation of the public function
  addFiles.
*/

void FolderNode::addFileNode(FileNode *file)
{
    Q_ASSERT(managingProject());

    QTC_ASSERT(!file->parentFolderNode(),
               qDebug("File node has already a parent folder"));

    file->setParentFolderNode(this);
    m_fileNodes.append(file);
}

/*!
  Sets file nodes specified by \a files to the internal list of the folder
  and emits the corresponding signals from the projectNode.
*/

void FolderNode::setFileNodes(const QList<FileNode *> &files)
{
    qDeleteAll(m_fileNodes);
    m_fileNodes = files;
    for (FileNode *node : m_fileNodes)
        node->setParentFolderNode(this);
}

/*!
  Adds a folder node specified by \a folder to the node hierarchy below
  \a parentFolder and emits the corresponding signals.
*/
void FolderNode::addFolderNode(FolderNode *folder)
{
    Q_ASSERT(managingProject());

    QTC_ASSERT(!folder->parentFolderNode(),
               qDebug("Project node has already a parent folder"));
    folder->setParentFolderNode(this);
    m_folderNodes.append(folder);
}

void FolderNode::setFolderNodes(const QList<FolderNode *> &folders)
{
    qDeleteAll(m_folderNodes);
    m_folderNodes = folders;
    for (FolderNode *node : m_folderNodes)
        node->setParentFolderNode(this);
}

void FolderNode::makeEmpty()
{
    setFolderNodes({});
    setFileNodes({});
}

bool FolderNode::showInSimpleTree() const
{
    return false;
}

/*!
  \class ProjectExplorer::VirtualFolderNode

  In-memory presentation of a virtual folder.
  Note that the node itself + all children (files and folders) are "managed" by the owning project.
  A virtual folder does not correspond to a actual folder on the file system. See for example the
  sources, headers and forms folder the QmakeProjectManager creates
  VirtualFolderNodes are always sorted before FolderNodes and are sorted according to their priority.

  \sa ProjectExplorer::FileNode, ProjectExplorer::ProjectNode
*/
VirtualFolderNode::VirtualFolderNode(const Utils::FileName &folderPath, int priority) :
    FolderNode(folderPath, NodeType::VirtualFolder, QString())
{
    setPriority(priority);
}

/*!
  \class ProjectExplorer::ProjectNode

  \brief The ProjectNode class is an in-memory presentation of a Project.

  A concrete subclass must implement the persistent data.

  \sa ProjectExplorer::FileNode, ProjectExplorer::FolderNode
*/

/*!
  Creates an uninitialized project node object.
  */
ProjectNode::ProjectNode(const Utils::FileName &projectFilePath) :
    FolderNode(projectFilePath, NodeType::Project)
{
    setPriority(DefaultProjectPriority);
    setDisplayName(projectFilePath.fileName());
}

QString ProjectNode::vcsTopic() const
{
    const QString dir = filePath().toFileInfo().absolutePath();

    if (Core::IVersionControl *const vc =
            Core::VcsManager::findVersionControlForDirectory(dir))
        return vc->vcsTopic(dir);

    return QString();
}

bool ProjectNode::canAddSubProject(const QString &proFilePath) const
{
    Q_UNUSED(proFilePath)
    return false;
}

bool ProjectNode::addSubProjects(const QStringList &proFilePaths)
{
    Q_UNUSED(proFilePaths)
    return false;
}

bool ProjectNode::removeSubProjects(const QStringList &proFilePaths)
{
    Q_UNUSED(proFilePaths)
    return false;
}

bool ProjectNode::addFiles(const QStringList &filePaths, QStringList *notAdded)
{
    Q_UNUSED(filePaths)
    Q_UNUSED(notAdded)
    return false;
}

bool ProjectNode::removeFiles(const QStringList &filePaths, QStringList *notRemoved)
{
    Q_UNUSED(filePaths)
    Q_UNUSED(notRemoved)
    return false;
}

bool ProjectNode::deleteFiles(const QStringList &filePaths)
{
    Q_UNUSED(filePaths)
    return false;
}

bool ProjectNode::canRenameFile(const QString &filePath, const QString &newFilePath)
{
    Q_UNUSED(filePath);
    Q_UNUSED(newFilePath);
    return true;
}

bool ProjectNode::renameFile(const QString &filePath, const QString &newFilePath)
{
    Q_UNUSED(filePath)
    Q_UNUSED(newFilePath)
    return false;
}

bool ProjectNode::deploysFolder(const QString &folder) const
{
    Q_UNUSED(folder);
    return false;
}

/*!
  \function bool ProjectNode::runConfigurations() const

  Returns a list of \c RunConfiguration suitable for this node.
  */
QList<RunConfiguration *> ProjectNode::runConfigurations() const
{
    return QList<RunConfiguration *>();
}

void ProjectNode::accept(NodesVisitor *visitor)
{
    visitor->visitProjectNode(this);

    foreach (FolderNode *folder, m_folderNodes)
        folder->accept(visitor);
}

ProjectNode *ProjectNode::projectNode(const Utils::FileName &file) const
{
    return Utils::findOrDefault(m_projectNodes, [&file](const ProjectNode *fn) {
        return fn->filePath() == file;
    });
}

QList<ProjectNode*> ProjectNode::projectNodes() const
{
    return m_projectNodes;
}

/*!
  Adds project nodes specified by \a subProjects to the node hierarchy and
  emits the corresponding signals.
  */
void ProjectNode::addProjectNode(ProjectNode *subProject)
{
    QTC_ASSERT(subProject, return);
    QTC_ASSERT(!subProject->parentFolderNode(), return);

    subProject->setParentFolderNode(this);
    m_folderNodes.append(subProject);
    m_projectNodes.append(subProject);

    Utils::sort(m_folderNodes);
    Utils::sort(m_projectNodes);
}


/*!
  Removes all child nodes from the node hierarchy and deletes them.
*/

void ProjectNode::makeEmpty()
{
    foreach (ProjectNode *subProject, m_projectNodes)
        m_folderNodes.removeAll(subProject);

    qDeleteAll(m_projectNodes);
    m_projectNodes.clear();

    FolderNode::makeEmpty();
}

bool ProjectNode::isEmpty() const
{
    return m_fileNodes.isEmpty() && m_folderNodes.isEmpty() && m_projectNodes.isEmpty();
}

/*!
  \class ProjectExplorer::SessionNode
*/

SessionNode::SessionNode() :
    FolderNode(Utils::FileName::fromString("session"), NodeType::Session)
{ }

QList<ProjectAction> SessionNode::supportedActions(Node *node) const
{
    Q_UNUSED(node)
    return QList<ProjectAction>();
}


void SessionNode::accept(NodesVisitor *visitor)
{
    visitor->visitSessionNode(this);
    foreach (ProjectNode *project, m_projectNodes)
        project->accept(visitor);
}

bool SessionNode::showInSimpleTree() const
{
    return true;
}

QList<ProjectNode*> SessionNode::projectNodes() const
{
    return m_projectNodes;
}

QString SessionNode::addFileFilter() const
{
    return QString::fromLatin1("*.c; *.cc; *.cpp; *.cp; *.cxx; *.c++; *.h; *.hh; *.hpp; *.hxx;");
}

void SessionNode::addProjectNode(ProjectNode *projectNode)
{
    QTC_ASSERT(!projectNode->parentFolderNode(),
               qDebug("Project node has already a parent folder"));
    projectNode->setParentFolderNode(this);
    m_folderNodes.append(projectNode);
    m_projectNodes.append(projectNode);

    Utils::sort(m_folderNodes);
    Utils::sort(m_projectNodes);
}

void SessionNode::removeProjectNode(ProjectNode *projectNode)
{
    m_folderNodes.removeOne(projectNode);
    m_projectNodes.removeOne(projectNode);
}

} // namespace ProjectExplorer
