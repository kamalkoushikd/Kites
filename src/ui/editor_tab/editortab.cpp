#include "editortab.h"
#include "custom_pseudo_manager/custom_pseudo_manager.h"
#include "ui/theme/theme_manager.h"
#include "ui_editortab.h"
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QTabBar>
#include <QTextStream>

namespace Kites
{

EditorTab::EditorTab(QWidget *parent, ProcessorManager *vmManager)
    : KitesTab(parent), m_processorManager(vmManager), ui(new Ui::EditorTab)
{
    ui->setupUi(this);
    ui->editorViewButton->setChecked(true); // default to editor view
    ui->stackedWidget->setCurrentIndex(0);  // show editor view by default

    ui->stackedWidget->layout()->setContentsMargins(0, 0, 0, 0);
    ui->stackedWidget->layout()->setSpacing(0);
    ui->stackedWidget->widget(0)->layout()->setContentsMargins(0, 0, 0, 0);
    ui->stackedWidget->widget(1)->layout()->setContentsMargins(0, 0, 0, 0);

    m_disassemblyView = ui->disassemblyTextEdit;
    m_expandedView = ui->expandedTextEdit;
    m_expandedView->setReadOnly(true);
    m_expandedView->setBreakpointInteractionEnabled(false);
    m_squiggleFormat.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    m_squiggleFormat.setUnderlineColor(ThemeManager::getInstance().getEditorErrorColor());
    connect(&ThemeManager::getInstance(), &ThemeManager::editorThemeChangedSignal, this,
            [this](const QString &)
            {
                m_squiggleFormat.setUnderlineColor(
                    ThemeManager::getInstance().getEditorErrorColor());
            });

    connect(ui->expandedViewButton, &QRadioButton::toggled, this,
            &EditorTab::onExpandButtonClicked);

    m_languageService = new LanguageService(this);
    connect(m_languageService, &LanguageService::diagnosticsReadySignal, this,
            [this](const QVector<Diagnostic> &diagnostics)
            {
                if (m_editor->isReadOnly())
                    return; // a Run started while this request was in flight; Run owns the
                            // squiggles now, don't clobber them with a stale live result
                applyDiagnostics(diagnostics);
            });

    m_diagnosticsDebounceTimer = new QTimer(this);
    m_diagnosticsDebounceTimer->setSingleShot(true);
    m_diagnosticsDebounceTimer->setInterval(300);
    connect(m_diagnosticsDebounceTimer, &QTimer::timeout, this,
            &EditorTab::requestLiveDiagnostics);
    // connect(m_diagnosticsDebounceTimer, &QTimer::timeout, this,
    //         [this]()
    //         {
                
    //         });

    connect(ui->documentTabs, &QTabWidget::currentChanged, this, &EditorTab::onCurrentDocumentChanged);
    connect(ui->documentTabs, &QTabWidget::tabCloseRequested, this,
            [this](int index) { closeDocument(index); });
    m_editor = addDocument();
    m_expandedView->setBreakpoints(m_editor->getBreakpoints());
}

Editor *EditorTab::addDocument()
{
    auto *editor = new Editor(ui->documentTabs);
    editor->setObjectName("assemblyTextEdit");
    editor->setPlaceholderText("Enter your code here...");
    editor->setReadOnly(m_editor && m_editor->isReadOnly());
    m_filePaths.insert(editor, QString());
    int number = 1; // the lowest one no other untitled document shows
    for (bool used = true; used; )
    {
        used = false;
        for (auto it = m_filePaths.cbegin(); it != m_filePaths.cend(); ++it)
            if (it.key() != editor && it.value().isEmpty() && it.key()->property("untitledNumber").toInt() == number)
                used = true;
        if (used)
            ++number;
    }
    editor->setProperty("untitledNumber", number);
    connect(editor, &QPlainTextEdit::textChanged, this,
            [this, editor]()
            {
                if (editor == m_editor)
                    m_diagnosticsDebounceTimer->start();
            });
    connect(editor->document(), &QTextDocument::modificationChanged, this,
            [this, editor](bool) { updateTabTitle(editor); });
    const int index = ui->documentTabs->addTab(editor, QString());
    updateTabTitle(editor);
    ui->documentTabs->setCurrentIndex(index);
    return editor;
}

Editor *EditorTab::editorAt(int index) const
{
    return dynamic_cast<Editor *>(ui->documentTabs->widget(index));
}

Editor *EditorTab::currentEditor() const
{
    return m_editor;
}

QString EditorTab::documentName(Editor *editor) const
{
    const QString path = m_filePaths.value(editor);
    if (!path.isEmpty())
        return QFileInfo(path).fileName();
    return QString("Untitled %1").arg(editor->property("untitledNumber").toInt());
}

void EditorTab::updateTabTitle(Editor *editor)
{
    const int index = ui->documentTabs->indexOf(editor);
    if (index < 0)
        return;
    const QString name = documentName(editor);
    ui->documentTabs->setTabText(index, editor->document()->isModified() ? name + " *" : name);
    ui->documentTabs->setTabToolTip(index, m_filePaths.value(editor));
}

void EditorTab::onCurrentDocumentChanged(int index)
{
    Editor *editor = editorAt(index);
    if (!editor || editor == m_editor)
        return;
    if (m_editor)
    {
        // the squiggles belong to the document that was shown
        m_editor->setExtraSelections({});
        m_editor->resetErrors();
    }
    m_editor = editor;
    m_expandedView->setBreakpoints(m_editor->getBreakpoints());
    m_disassemblyView->setPlainText(QString()); // it showed the previous document's program
    if (ui->stackedWidget->currentIndex() == 1)
        onExpandButtonClicked(true);
    requestLiveDiagnostics();
    emit currentEditorChanged(m_editor);
}

void EditorTab::newDocument()
{
    if (!m_editor->isReadOnly()) // a program is running from the current document
        addDocument();
}

bool EditorTab::openFile(const QString &filePath)
{
    if (m_editor->isReadOnly())
        return false; // a program is running from the current document
    const QString absolute = QFileInfo(filePath).absoluteFilePath();
    for (int i = 0; i < ui->documentTabs->count(); ++i)
    {
        if (m_filePaths.value(editorAt(i)) == absolute)
        {
            ui->documentTabs->setCurrentIndex(i); // already open
            return true;
        }
    }

    QFile file(absolute);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, "Open File", "Could not open " + absolute + ":\n" + file.errorString());
        return false;
    }
    const QString content = QTextStream(&file).readAll();

    // an empty, unchanged, never saved document is replaced instead of kept next to the file
    Editor *editor = m_editor;
    if (!(m_filePaths.value(editor).isEmpty() && !editor->document()->isModified() &&
          editor->toPlainText().isEmpty()))
    {
        editor = addDocument();
    }
    editor->setPlainText(content);
    editor->document()->setModified(false);
    m_filePaths[editor] = absolute;
    updateTabTitle(editor);
    return true;
}

bool EditorTab::saveDocument(Editor *editor, QString filePath)
{
    if (filePath.isEmpty())
    {
        filePath = QFileDialog::getSaveFileName(this, "Save File", documentName(editor),
                                                "Assembly Files (*.asm *.s);;All Files (*)");
        if (filePath.isEmpty())
            return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, "Save File", "Could not save " + filePath + ":\n" + file.errorString());
        return false;
    }
    QTextStream(&file) << editor->toPlainText();
    m_filePaths[editor] = QFileInfo(filePath).absoluteFilePath();
    editor->document()->setModified(false);
    updateTabTitle(editor);
    return true;
}

bool EditorTab::save()
{
    return saveDocument(m_editor, m_filePaths.value(m_editor));
}

bool EditorTab::saveAs()
{
    return saveDocument(m_editor, QString());
}

bool EditorTab::confirmClose(Editor *editor)
{
    if (!editor->document()->isModified())
        return true;
    ui->documentTabs->setCurrentWidget(editor);
    const auto answer = QMessageBox::question(
        this, "Unsaved Changes", documentName(editor) + " has unsaved changes. Save them?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return saveDocument(editor, m_filePaths.value(editor));
    return answer == QMessageBox::Discard;
}

bool EditorTab::closeDocument(int index)
{
    Editor *editor = editorAt(index);
    if (!editor || m_editor->isReadOnly() || !confirmClose(editor))
        return false; // no closing while a program runs
    if (ui->documentTabs->count() == 1)
        addDocument(); // there is always a document to type in
    ui->documentTabs->removeTab(ui->documentTabs->indexOf(editor));
    m_filePaths.remove(editor);
    editor->deleteLater();
    return true;
}

bool EditorTab::closeCurrentDocument()
{
    return closeDocument(ui->documentTabs->currentIndex());
}

bool EditorTab::closeAllDocuments()
{
    for (int i = 0; i < ui->documentTabs->count(); ++i)
    {
        if (!confirmClose(editorAt(i)))
            return false;
    }
    return true;
}

void EditorTab::requestLiveDiagnostics()
{
    if (m_editor->isReadOnly())
        return; // running/debugging - Run's own diagnostics own the squiggles right now
    m_languageService->requestDiagnostics(QString::fromStdString(getRawText()));
}

void EditorTab::onExpandButtonClicked(bool checked)
{
    if (m_expandedLocked && !checked)
    {
        ui->expandedViewButton->setChecked(true);
        return;
    }

    if (checked)
    {
        std::string expandedCode = customPseudoManager::expandPseudoInstruction(getRawText());
        m_expandedView->setPlainText(QString::fromStdString(expandedCode));
        m_expandedView->setBreakpoints(m_editor->getBreakpoints());
        ui->stackedWidget->setCurrentIndex(1);
    }
    else
    {
        ui->stackedWidget->setCurrentIndex(0);
    }
}

void EditorTab::switchToExpandedView()
{
    ui->expandedViewButton->setChecked(true);
}

void EditorTab::switchToEditorView()
{
    ui->editorViewButton->setChecked(true);
}

void EditorTab::setExpandedLocked(bool locked)
{
    m_expandedLocked = locked;
    ui->editorViewButton->setEnabled(!locked);

    if (locked)
    {
        ui->expandedViewButton->setChecked(true);
        ui->stackedWidget->setCurrentIndex(1);
    }
}

EditorTab::~EditorTab()
{
    delete ui;
}

std::string EditorTab::getRawText()
{
    return m_editor->toPlainText().toStdString();
}

std::vector<uint64_t> EditorTab::getBreakpoints() const
{
    return m_editor->getBreakpoints();
}

void EditorTab::setRawText(const QString &text)
{
    m_editor->setPlainText(text);
}

void EditorTab::resetErrorLines()
{
    m_editor->setExtraSelections({});
    m_editor->resetErrors();
}

void EditorTab::updateDisassemblyView(const std::string &disassembledCode)
{
    m_disassemblyView->setPlainText(QString::fromStdString(disassembledCode));
}

void EditorTab::highlightLines(const std::vector<std::pair<int,std::string>> &editorLines, 
const std::vector<std::pair<int,std::string>> &disassemblyLines)
{
    m_editor->setLinesToHighlight(editorLines);
    m_disassemblyView->setLinesToHighlight(disassemblyLines);
    m_expandedView->setLinesToHighlight(
        editorLines); // we want the expanded view to have the same highlights as the editor
}

void EditorTab::setCanWrite(bool canWrite)
{
    // the running program belongs to the current document: keep it shown until the run ends
    for (int i = 0; i < ui->documentTabs->count(); ++i)
        editorAt(i)->setReadOnly(!canWrite);
    ui->documentTabs->tabBar()->setEnabled(canWrite);
}

void EditorTab::showRuntimeError(int line, const QString &message)
{
    if (line <= 0)
        return;
    m_editor->setLinesToHighlight({{line, "Runtime Error"}});
    m_editor->setErrorMessage(line - 1, message);
}

void EditorTab::clearHighlights()
{
    m_editor->clearHighlights();
    m_expandedView->clearHighlights();
    m_disassemblyView->clearHighlights();
}

void EditorTab::setErrorLinesFromFile(const std::filesystem::path &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        qWarning() << "Could not open error file:" << filePath.string();
        return;
    }

    QByteArray fileData = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(fileData);

    if (doc.isNull() || !doc.isObject())
    {
        qWarning() << "Failed to parse error JSON.";
        return;
    }

    QJsonObject rootObject = doc.object();
    QJsonArray errors = rootObject["errors"].toArray();

    QVector<Diagnostic> diagnostics;
    diagnostics.reserve(errors.size());
    for (const QJsonValue &value : errors)
    {
        QJsonObject errorObj = value.toObject();
        diagnostics.append(
            Diagnostic{errorObj["line"].toInt(), 0, errorObj["message"].toString()});
    }

    applyDiagnostics(diagnostics);
}

void EditorTab::applyDiagnostics(const QVector<Diagnostic> &diagnostics)
{
    m_editor->resetErrors();

    QList<QTextEdit::ExtraSelection> extraSelections;
    for (const Diagnostic &diagnostic : diagnostics)
    {
        if (diagnostic.line <= 0)
            continue; // Skip invalid lines

        QTextBlock block = m_editor->document()->findBlockByNumber(
            diagnostic.line - 1); // -1 since line is 1 based
        if (!block.isValid())
        {
            continue; // Line number is out of bounds
        }
        QTextEdit::ExtraSelection selection;
        selection.format = m_squiggleFormat;
        m_editor->setErrorMessage(diagnostic.line - 1,
                                  diagnostic.message); // passing error to editor for tooltip display

        QTextCursor cursor(block);
        cursor.movePosition(QTextCursor::StartOfLine);
        cursor.movePosition(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
        selection.cursor = cursor;
        extraSelections.append(selection);
    }

    m_editor->setExtraSelections(extraSelections);
    m_expandedView->setExtraSelections(extraSelections);
}
} // namespace Kites
