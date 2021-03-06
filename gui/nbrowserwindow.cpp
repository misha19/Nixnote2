/*********************************************************************************
NixNote - An open-source client for the Evernote service.
Copyright (C) 2013 Randy Baumgarte

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
***********************************************************************************/

#include "nbrowserwindow.h"
#include "sql/notetable.h"
#include "sql/notebooktable.h"
#include "gui/browserWidgets/urleditor.h"
#include "sql/tagtable.h"
#include "html/noteformatter.h"
#include "html/enmlformatter.h"
#include "sql/usertable.h"
#include "sql/resourcetable.h"
#include "sql/linkednotebooktable.h"
#include "global.h"
#include "gui/browserWidgets/colormenu.h"
#include "gui/plugins/pluginfactory.h"
#include "dialog/insertlinkdialog.h"
#include "html/thumbnailer.h"
#include "dialog/tabledialog.h"
#include "dialog/insertlatexdialog.h"
#include "dialog/encryptdialog.h"
#include "sql/configstore.h"
#include "utilities/encrypt.h"
#include "utilities/mimereference.h"
#include "html/attachmenticonbuilder.h"

#include <QVBoxLayout>
#include <QAction>
#include <QMenu>
#include <QFileIconProvider>
#include <QFontDatabase>
#include <QSplitter>
#include <QDesktopServices>
#include <QMessageBox>
#include <QFileDialog>
#include <QClipboard>
#include <QBuffer>
#include <QDateTime>
#include <iostream>
#include <istream>



extern Global global;

NBrowserWindow::NBrowserWindow(QWidget *parent) :
    QWidget(parent)
{

//    this->setStyleSheet("margins:0px;");
    QHBoxLayout *line1Layout = new QHBoxLayout();
    QVBoxLayout *layout = new QVBoxLayout();   // Note content layout

    // Setup line #1 of the window.  The text & notebook
    layout->addLayout(line1Layout);
    line1Layout->addWidget(&noteTitle);
    line1Layout->addWidget(&notebookMenu);
    line1Layout->addWidget(&expandButton);


    // Add the third layout display (which actually appears on line 2)
    layout->addLayout(&line3Layout);
    line3Layout.addWidget(&dateEditor);

    // Add the second layout display (which actually appears on line 3)
    layout->addLayout(&line2Layout);
    line2Layout.addWidget(&urlEditor,1);
    line2Layout.addWidget(&tagEditor, 3);

    editor = new NWebView(this);
    editor->setTitleEditor(&noteTitle);
    setupToolBar();
    layout->addWidget(buttonBar);

    // setup the source editor
    sourceEdit = new QTextEdit(this);
    sourceEdit->setVisible(false);
    sourceEdit->setTabChangesFocus(true);
    //sourceEdit->setLineWrapMode(QTextEdit::LineWrapMode);
    QFont font;
    font.setFamily("Courier");
    font.setFixedPitch(true);
    font.setPointSize(10);
    sourceEdit->setFont(font);
    XmlHighlighter *highlighter = new XmlHighlighter(sourceEdit->document());
    highlighter = highlighter;  // Prevents the unused warning
    sourceEditorTimer = new QTimer();
    connect(sourceEditorTimer, SIGNAL(timeout()), this, SLOT(setSource()));

    // addthe actual note editor & source view
    QSplitter *editorSplitter = new QSplitter(Qt::Vertical, this);
    editorSplitter->addWidget(editor);
    editorSplitter->addWidget(sourceEdit);
    layout->addWidget(editorSplitter);
    setLayout(layout);
    layout->setMargin(0);

    // Setup shortcuts
    focusNoteShortcut = new QShortcut(this);
    setupShortcut(focusNoteShortcut, "Focus_Note");
    connect(focusNoteShortcut, SIGNAL(activated()), this, SLOT(focusNote()));
    focusTitleShortcut = new QShortcut(this);
    setupShortcut(focusTitleShortcut, "Focus_Title");
    connect(focusTitleShortcut, SIGNAL(activated()), this, SLOT(focusTitle()));
    insertDatetimeShortcut = new QShortcut(this);
    setupShortcut(insertDatetimeShortcut, "Insert_DateTime");
    connect(insertDatetimeShortcut, SIGNAL(activated()), this, SLOT(insertDatetime()));


    // Setup the signals
    connect(&expandButton, SIGNAL(stateChanged(int)), this, SLOT(changeExpandState(int)));
    connect(&notebookMenu, SIGNAL(notebookChanged()), this, SLOT(sendUpdateSignal()));
    connect(&urlEditor, SIGNAL(textUpdated()), this, SLOT(sendUpdateSignal()));
    connect(&noteTitle, SIGNAL(titleChanged()), this, SLOT(sendUpdateSignal()));
    connect(&dateEditor, SIGNAL(valueChanged()), this, SLOT(sendUpdateSignal()));
    connect(&tagEditor, SIGNAL(tagsUpdated()), this, SLOT(sendUpdateSignal()));
    connect(&tagEditor, SIGNAL(newTagCreated(qint32)), this, SLOT(newTagAdded(qint32)));
    connect(editor, SIGNAL(noteChanged()), this, SLOT(noteContentUpdated()));
    connect(sourceEdit, SIGNAL(textChanged()), this, SLOT(noteSourceUpdated()));

    connect(editor->page(), SIGNAL(linkClicked(QUrl)), this, SLOT(linkClicked(QUrl)));
    connect(editor->page(), SIGNAL(microFocusChanged()), this, SLOT(microFocusChanged()));

    editor->page()->setLinkDelegationPolicy(QWebPage::DelegateAllLinks);
    connect(editor->page()->mainFrame(), SIGNAL(javaScriptWindowObjectCleared()), this, SLOT(exposeToJavascript()));
    connect(editor->page()->mainFrame(), SIGNAL(javaScriptWindowObjectCleared()), editor, SLOT(exposeToJavascript()));

    editor->page()->settings()->setAttribute(QWebSettings::PluginsEnabled, true);
    factory = new PluginFactory(this);
    editor->page()->setPluginFactory(factory);

    buttonBar->setupVisibleButtons();

    hammer = new Thumbnailer(global.db);
    lid = -1;
}



// Setup the toolbar window of the editor
void NBrowserWindow::setupToolBar() {
    buttonBar = new EditorButtonBar();

    // Toolbar action
    connect(buttonBar->undoButtonAction, SIGNAL(triggered()), this, SLOT(undoButtonPressed()));
    connect(buttonBar->redoButtonAction, SIGNAL(triggered()), this, SLOT(redoButtonPressed()));
    connect(buttonBar->cutButtonAction, SIGNAL(triggered()), this, SLOT(cutButtonPressed()));
    connect(buttonBar->copyButtonAction, SIGNAL(triggered()), this, SLOT(copyButtonPressed()));
    connect(buttonBar->pasteButtonAction, SIGNAL(triggered()), this, SLOT(pasteButtonPressed()));
    connect(buttonBar->removeFormatButtonAction, SIGNAL(triggered()), this, SLOT(removeFormatButtonPressed()));
    connect(buttonBar->boldButtonWidget, SIGNAL(clicked()), this, SLOT(boldButtonPressed()));
    connect(buttonBar->italicButtonWidget, SIGNAL(clicked()), this, SLOT(italicsButtonPressed()));
    connect(buttonBar->underlineButtonWidget, SIGNAL(clicked()), this, SLOT(underlineButtonPressed()));
    connect(buttonBar->leftJustifyButtonAction, SIGNAL(triggered()), this, SLOT(alignLeftButtonPressed()));
    connect(buttonBar->rightJustifyButtonAction, SIGNAL(triggered()), this, SLOT(alignRightButtonPressed()));
    connect(buttonBar->centerJustifyButtonAction, SIGNAL(triggered()), this, SLOT(alignCenterButtonPressed()));
    connect(buttonBar->strikethroughButtonAction, SIGNAL(triggered()), this, SLOT(strikethroughButtonPressed()));
    connect(buttonBar->hlineButtonAction, SIGNAL(triggered()), this, SLOT(horizontalLineButtonPressed()));
    connect(buttonBar->shiftRightButtonAction, SIGNAL(triggered()), this, SLOT(shiftRightButtonPressed()));
    connect(buttonBar->shiftLeftButtonAction, SIGNAL(triggered()), this, SLOT(shiftLeftButtonPressed()));
    connect(buttonBar->bulletListButtonAction, SIGNAL(triggered()), this, SLOT(bulletListButtonPressed()));
    connect(buttonBar->numberListButtonAction, SIGNAL(triggered()), this, SLOT(numberListButtonPressed()));
    connect(buttonBar->todoButtonAction, SIGNAL(triggered()), this, SLOT(todoButtonPressed()));
    connect(buttonBar->fontSizes, SIGNAL(currentIndexChanged(int)), this, SLOT(fontSizeSelected(int)));
    connect(buttonBar->fontNames, SIGNAL(currentIndexChanged(int)), this, SLOT(fontNameSelected(int)));
    connect(buttonBar->fontColorButtonWidget, SIGNAL(clicked()), this, SLOT(fontColorClicked()));
    connect(buttonBar->fontColorMenuWidget->getMenu(), SIGNAL(triggered(QAction*)), this, SLOT(fontColorClicked()));
    connect(buttonBar->highlightColorButtonWidget, SIGNAL(clicked()), this, SLOT(fontHighlightClicked()));
    connect(buttonBar->highlightColorMenuWidget->getMenu(), SIGNAL(triggered(QAction*)), this, SLOT(fontHighlightClicked()));
    connect(buttonBar->insertTableButtonAction, SIGNAL(triggered()), this, SLOT(insertTableButtonPressed()));
}




// Load any shortcut keys
void NBrowserWindow::setupShortcut(QShortcut *action, QString text) {
    if (!global.shortcutKeys->containsAction(&text))
        return;
    QKeySequence key(global.shortcutKeys->getShortcut(&text));
    action->setKey(key);
}


// Load the note content into the window
void NBrowserWindow::setContent(qint32 lid) {

    //hammer->timer.stop();
    // First, make sure we have a valid lid
    if (lid == -1) {       
        editor->page()->setContentEditable(false);
        clear();
        return;
    }

    // If we are already updating this note, we don't do anything
    if (lid == this->lid)
        return;

//    if (this->editor->isDirty)
//        this->saveNoteContent();

    // let's load the new note
    this->lid = lid;
    this->editor->isDirty = false;

    NoteTable noteTable(global.db);
    Note n;

    bool rc = noteTable.get(n, this->lid, false, false);
    if (!rc)
        return;

    QByteArray content;

    bool inkNote;
    bool readOnly;

    // If we are searching, we never pull from the cache since the search string may
    // have changed since the last time.
    FilterCriteria *criteria = global.filterCriteria[global.filterPosition];
    if (criteria->isSearchStringSet() && criteria->getSearchString().trimmed() != "")
        global.cache.remove(lid);

    if (!global.cache.contains(lid)) {
        NoteFormatter formatter;
        if (criteria->isSearchStringSet())
            formatter.setHighlightText(criteria->getSearchString());
        formatter.setNote(n, global.pdfPreview);
        formatter.setHighlight();
        content = formatter.rebuildNoteHTML();
        if (!criteria->isSearchStringSet()) {
            NoteCache *newCache = new NoteCache();
            newCache->isReadOnly = formatter.readOnly;
            newCache->isInkNote = formatter.inkNote;
            newCache->noteContent = content;
            global.cache.insert(lid, newCache);
        }
        readOnly = formatter.readOnly;
        inkNote = formatter.inkNote;
    } else {
        NoteCache *c = global.cache[lid];
        content = c->noteContent;
        readOnly = c->isReadOnly;
        inkNote = c->isInkNote;
    }

    setReadOnly(readOnly);

    noteTitle.setTitle(lid, QString::fromStdString(n.title), QString::fromStdString(n.title));
    dateEditor.setNote(lid, n);
    QLOG_DEBUG() << content;
    //editor->setContent(content,  "application/xhtml+xml");
    QWebSettings::setMaximumPagesInCache(0);
    QWebSettings::setObjectCacheCapacities(0, 0, 0);
    editor->setContent(content);
    // is this an ink note?
    if (inkNote)
        editor->page()->setContentEditable(false);

    // Set the tag names
    tagEditor.clear();
    QStringList names;
    for (unsigned int i=0; i<n.tagNames.size(); i++) {
        names << QString::fromStdString(n.tagNames[i]);
    }
    tagEditor.setTags(names);
    tagEditor.setCurrentLid(lid);
    NotebookTable notebookTable(global.db);
    qint32 notebookLid = notebookTable.getLid(n.notebookGuid);
    LinkedNotebookTable linkedTable(global.db);
    if (linkedTable.exists(notebookLid))
        tagEditor.setAccount(notebookLid);
    else
        tagEditor.setAccount(0);

    this->lid = lid;
    notebookMenu.setCurrentNotebook(lid, n);
    if (n.__isset.attributes && n.attributes.__isset.sourceURL)
        urlEditor.setUrl(lid, QString::fromStdString(n.attributes.sourceURL));
    else
        urlEditor.setUrl(lid, "");
    setSource();

    if (criteria->isSearchStringSet()) {
        QStringList list = criteria->getSearchString().split(" ");
        for (int i=0; i<list.size(); i++) {
            editor->page()->findText(list[i], QWebPage::HighlightAllOccurrences);
        }
    }

    if (hammer->idle && noteTable.isThumbnailNeeded(this->lid)) {
        hammer->render(this->lid);
    } /*else
        hammer->timer.start(1000);*/

}


void NBrowserWindow::setReadOnly(bool readOnly) {
    if (readOnly) {
        noteTitle.setFocusPolicy(Qt::NoFocus);
        tagEditor.setEnabled(false);
        authorEditor.setFocusPolicy(Qt::NoFocus);
        locationEditor.setFocusPolicy(Qt::NoFocus);
        urlEditor.setFocusPolicy(Qt::NoFocus);
        notebookMenu.setEnabled(false);
        dateEditor.setEnabled(false);
        editor->page()->setContentEditable(false);
        return;
    }
    noteTitle.setFocusPolicy(Qt::StrongFocus);
    tagEditor.setEnabled(true);
    authorEditor.setFocusPolicy(Qt::StrongFocus);
    locationEditor.setFocusPolicy(Qt::StrongFocus);
    urlEditor.setFocusPolicy(Qt::StrongFocus);
    notebookMenu.setEnabled(true);
    dateEditor.setEnabled(true);
    editor->page()->setContentEditable(true);

}




// Show / hide various note attributes depending upon what the user
// has clicked
void NBrowserWindow::changeExpandState(int value) {
    switch (value) {
    case EXPANDBUTTON_1:
        urlEditor.hide();
        tagEditor.hide();
        dateEditor.hide();
        break;
    case EXPANDBUTTON_2:
        urlEditor.show();
        tagEditor.show();
        break;
    case EXPANDBUTTON_3:
        dateEditor.show();
        break;
    }
}



// Send a signal that the note has been updated
void NBrowserWindow::sendUpdateSignal() {
    emit(this->noteUpdated(lid));
    emit(this->updateNoteList(lid, NOTE_TABLE_TITLE_POSITION, this->noteTitle.text()));
}



// Send a signal that a tag has been added to a note
void NBrowserWindow::newTagAdded(qint32 lid) {
    emit(tagAdded(lid));
}



// Add a tag to a note
void NBrowserWindow::addTagName(qint32 lid) {
    TagTable table(global.db);
    Tag t;
    table.get(t, lid);
    tagEditor.addTag(QString::fromStdString(t.name));
}




// Rename a tag in a note.
void NBrowserWindow::tagRenamed(qint32 lid, QString oldName, QString newName) {
    tagEditor.tagRenamed(lid, oldName, newName);
}



// Remove a tag in a note
void NBrowserWindow::tagDeleted(qint32 lid, QString name) {
    lid = lid;  /* suppress unused */
    tagEditor.removeTag(name);
}



// A notebook was renamed
void NBrowserWindow::notebookRenamed(qint32 lid, QString oldName, QString newName) {
    lid = lid;  /* suppress unused */
    oldName = oldName;  /* suppress unused */
    newName = newName;  /* suppress unused */
    notebookMenu.reloadData();
}




// A notebook was deleted
void NBrowserWindow::notebookDeleted(qint32 lid, QString name) {
    lid = lid;  /* suppress unused */
    name=name;  /* suppress unused */
    notebookMenu.reloadData();
}



// A stack was renamed
void NBrowserWindow::stackRenamed(QString oldName, QString newName) {
    oldName = oldName;  /* suppress unused */
    newName = newName;  /* suppress unused */
    notebookMenu.reloadData();
}



// A stack was deleted
void NBrowserWindow::stackDeleted(QString name) {
    name=name;  /* suppress unused */
    notebookMenu.reloadData();
}



// A stack was added
void NBrowserWindow::stackAdded(QString name) {
    name=name;  /* suppress unused */
    notebookMenu.reloadData();
}



// A notebook was added
void NBrowserWindow::notebookAdded(qint32 lid) {
    lid = lid;  /* suppress unused */
    notebookMenu.reloadData();
}


// A note was synchronized with Evernote's servers
void NBrowserWindow::noteSyncUpdate(qint32 lid) {
    if (lid != this->lid || editor->isDirty)
        return;
    setContent(lid);
}




// A note's content was updated
void NBrowserWindow::noteContentUpdated() {
    if (editor->isDirty) {
        NoteTable noteTable(global.db);
        noteTable.setDirty(this->lid, true);
        emit(noteUpdated(this->lid));
    }
    if (sourceEdit->isVisible()) {
        sourceEditorTimer->stop();
        sourceEditorTimer->setInterval(500);
        sourceEditorTimer->setSingleShot(true);
        sourceEditorTimer->start();
    }
}




// Save the note's content
void NBrowserWindow::saveNoteContent() {
    if (this->editor->isDirty) {
        NoteTable table(global.db);
        //QString contents = editor->editorPage->mainFrame()->toHtml();
        QString contents = editor->editorPage->mainFrame()->documentElement().toOuterXml();
        EnmlFormatter formatter;
        formatter.setHtml(contents);
        formatter.rebuildNoteEnml();
        if (formatter.formattingError) {
            QMessageBox::information(this, tr("Unable to Save"), QString(tr("Unable to save this note.  Either tidy isn't installed or the note is too complex to save.")));
            return;
        }


        // get a list of lids found in the note.
        // Purge anything that is no longer needed.
        QList<qint32> validLids = formatter.resources;
        QList<qint32> oldLids;
        ResourceTable resTable(global.db);
        resTable.getResourceList(oldLids, lid);

        for (int i=0; i<oldLids.size(); i++) {
            if (!validLids.contains(oldLids[i])) {
                resTable.expunge(oldLids[i]);
            }
        }



        table.updateNoteContent(lid, formatter.getEnml());
        Thumbnailer thumbnailer(global.db);
        thumbnailer.render(lid);

        NoteCache* cache = global.cache[lid];
        if (cache != NULL) {
            QByteArray b;
            b.append(contents);
            cache->noteContent = b;
            global.cache.remove(lid);
            global.cache.insert(lid, cache);
        }

        // Make sure the thumnailer is done
        while(!thumbnailer.idle);
    }
}



// The undo edit button was pressed
void NBrowserWindow::undoButtonPressed() {
    this->editor->triggerPageAction(QWebPage::Undo);
    this->editor->setFocus();
    microFocusChanged();
}



// The redo edit button was pressed
void NBrowserWindow::redoButtonPressed() {
    this->editor->triggerPageAction(QWebPage::Redo);
    this->editor->setFocus();
    microFocusChanged();
}


// The cut button was pressed
void NBrowserWindow::cutButtonPressed() {
    this->editor->triggerPageAction(QWebPage::Cut);
    this->editor->setFocus();
    microFocusChanged();
}


// The copy button was pressed
void NBrowserWindow::copyButtonPressed() {
    this->editor->triggerPageAction(QWebPage::Copy);
    this->editor->setFocus();
    microFocusChanged();
}


// The paste button was pressed
void NBrowserWindow::pasteButtonPressed() {
    if (forceTextPaste) {
        pasteWithoutFormatButtonPressed();
        return;
    }

    QClipboard *clipboard = global.clipboard;
    const QMimeData *mime = clipboard->mimeData();

    if (mime->hasImage()) {
        editor->setFocus();
        insertImage(mime);
        editor->setFocus();
        return;
    }

    if (mime->hasUrls()) {
        QList<QUrl> urls = mime->urls();
        for (int i=0; i<urls.size(); i++) {
            if (urls[i].toString().startsWith("file://")) {
                QString fileName = urls[i].toString().mid(7);
                attachFileSelected(fileName);
//                addAttachment(fileName);
                this->editor->triggerPageAction(QWebPage::InsertParagraphSeparator);
            }
        }
        this->editor->setFocus();
        microFocusChanged();
        return;
    }
    QLOG_DEBUG() << "HTML:" << mime->hasHtml() << " " << mime->html();
    QLOG_DEBUG() << "Color:" << mime->hasColor();
    QLOG_DEBUG() << "Url:" << mime->hasUrls();

    if (mime->hasText()) {
        QString urltext = mime->text();
        if (urltext.toLower().mid(0,17) == "evernote:///view/") {
            urltext = urltext.mid(17);
            int pos = urltext.indexOf("/");
            QString userid = urltext.mid(0,pos-1);
            urltext = urltext.mid(pos+1);
            pos = urltext.indexOf("/");
            QString shard = urltext.mid(0,pos);
            urltext = urltext.mid(pos+1);
            pos = urltext.indexOf("/");
            QString uid = urltext.mid(0,pos);
            urltext = urltext.mid(pos+1);
            pos = urltext.indexOf("/");
            QString guid = urltext.mid(0,pos);
            urltext = urltext.mid(pos);
            pos = urltext.indexOf("/");
            QString locguid = urltext.mid(pos);
            QString linkedNotebookGuid = urltext.mid(pos);

            Note n;
            bool goodrc = false;
            NoteTable ntable(global.db);
            goodrc = ntable.get(n, guid,false,false);
            if (!goodrc)
                goodrc = ntable.get(n,locguid,false,false);

            // If we have a good return, then we can paste the link, otherwise we fall out
            // to a normal paste.
            if (goodrc) {
                QString url = QString("<a href=\"") +global.clipboard->text()
                        +QString("\" title=") +QString::fromStdString(n.title)
                        +QString(" >") +QString::fromStdString(n.title) +QString("</a>");
                QString script = QString("document.execCommand('insertHtml', false, '")+url+QString("');");
                editor->page()->mainFrame()->evaluateJavaScript(script);
                return;
            }
        }
    }


    this->editor->triggerPageAction(QWebPage::Paste);
    this->editor->setFocus();
    microFocusChanged();
}




// The paste button was pressed
void NBrowserWindow::selectAllButtonPressed() {
    this->editor->triggerPageAction(QWebPage::SelectAll);
    this->editor->setFocus();
    microFocusChanged();
}



// The paste without mime format was pressed
void NBrowserWindow::pasteWithoutFormatButtonPressed() {
    const QMimeData *mime = global.clipboard->mimeData();
    if (!mime->hasText())
        return;
    QString text = mime->text();
    global.clipboard->clear();
    global.clipboard->setText(text, QClipboard::Clipboard);
    this->editor->triggerPageAction(QWebPage::Paste);

    // This is done because pasting into an encryption block
    // can cause multiple cells (which can't happen).  It
    // just goes through the table, extracts the data, &
    // puts it back as one table cell.
    if (insideEncryption) {
        QString js = QString( "function fixEncryption() { ")
                +QString("   var selObj = window.getSelection();")
                +QString("   var selRange = selObj.getRangeAt(0);")
                +QString("   var workingNode = window.getSelection().anchorNode;")
                         +QString("   while(workingNode != null && workingNode.nodeName.toLowerCase() != 'table') { ")
                +QString("           workingNode = workingNode.parentNode;")
                +QString("   } ")
                +QString("   workingNode.innerHTML = window.browserWindow.fixEncryptionPaste(workingNode.innerHTML);")
                +QString("} fixEncryption();");
        editor->page()->mainFrame()->evaluateJavaScript(js);
    }

    this->editor->setFocus();
    microFocusChanged();
}

// This basically removes all the table tags and returns just the contents.
// This is called by JavaScript to fix encryption pastes.
QString NBrowserWindow::fixEncryptionPaste(QString data) {
    data = data.replace("<tbody>", "");
    data = data.replace("</tbody>", "");
    data = data.replace("<tr>", "");
    data = data.replace("</tr>", "");
    data = data.replace("<td>", "");
    data = data.replace("</td>", "<br>");
    data = data.replace("<br><br>", "<br>");
    return QString("<tbody><tr><td>")+data+QString("</td></tr></tbody>");
}



// The bold button was pressed / toggled
void NBrowserWindow::boldButtonPressed() {
    this->editor->triggerPageAction(QWebPage::ToggleBold);
    this->editor->setFocus();
    microFocusChanged();
}



// The toggled button was pressed/toggled
void NBrowserWindow::italicsButtonPressed() {
    this->editor->triggerPageAction(QWebPage::ToggleItalic);
    this->editor->setFocus();
    microFocusChanged();
}


// The underline button was toggled
void NBrowserWindow::underlineButtonPressed() {
    this->editor->triggerPageAction(QWebPage::ToggleUnderline);
    this->editor->setFocus();
    microFocusChanged();
}



// The underline button was toggled
void NBrowserWindow::removeFormatButtonPressed() {
    this->editor->triggerPageAction(QWebPage::RemoveFormat);
    this->editor->setFocus();
    microFocusChanged();
}



// The strikethrough button was pressed
void NBrowserWindow::strikethroughButtonPressed() {
    this->editor->triggerPageAction(QWebPage::ToggleStrikethrough);
    this->editor->setFocus();
    microFocusChanged();
}



// The horizontal line button was pressed
void NBrowserWindow::horizontalLineButtonPressed() {
    this->editor->page()->mainFrame()->evaluateJavaScript(
            "document.execCommand('insertHorizontalRule', false, '');");
    editor->setFocus();
    microFocusChanged();
}



// The center align button was pressed
void NBrowserWindow::alignCenterButtonPressed() {
    this->editor->page()->mainFrame()->evaluateJavaScript(
            "document.execCommand('JustifyCenter', false, '');");
    editor->setFocus();
    microFocusChanged();
}



// The left allign button was pressed
void NBrowserWindow::alignLeftButtonPressed() {
    this->editor->page()->mainFrame()->evaluateJavaScript(
            "document.execCommand('JustifyLeft', false, '');");
    editor->setFocus();
    microFocusChanged();
}



// The allign right button was pressed
void NBrowserWindow::alignRightButtonPressed() {
    this->editor->page()->mainFrame()->evaluateJavaScript(
            "document.execCommand('JustifyRight', false, '');");
    editor->setFocus();
    microFocusChanged();
}



// The shift right button was pressed
void NBrowserWindow::shiftRightButtonPressed() {
    this->editor->page()->mainFrame()->evaluateJavaScript(
            "document.execCommand('indent', false, '');");
    editor->setFocus();
    microFocusChanged();
}



// The shift left button was pressed
void NBrowserWindow::shiftLeftButtonPressed() {
    this->editor->page()->mainFrame()->evaluateJavaScript(
            "document.execCommand('outdent', false, '');");
    editor->setFocus();
    microFocusChanged();
}




// The number list button was pressed
void NBrowserWindow::numberListButtonPressed() {
    this->editor->page()->mainFrame()->evaluateJavaScript(
            "document.execCommand('InsertOrderedList', false, '');");
    editor->setFocus();
    microFocusChanged();
}



// The bullet list button was pressed
void NBrowserWindow::bulletListButtonPressed() {
    this->editor->page()->mainFrame()->evaluateJavaScript(
            "document.execCommand('InsertUnorderedList', false, '');");
    editor->setFocus();
    microFocusChanged();
}


void NBrowserWindow::contentChanged() {
    this->editor->isDirty = true;
    saveNoteContent();
    this->sendUpdateSignal();
}

// The todo button was pressed
void NBrowserWindow::todoButtonPressed() {
    QString script_start="document.execCommand('insertHtml', false, '";
    QString script_end = "');";
    QString todo =
            "<input TYPE=\"CHECKBOX\" " +
            QString("onMouseOver=\"style.cursor=\\'hand\\'\" ") +
            QString("onClick=\"if(!checked) removeAttribute(\\'checked\\'); else setAttribute(\\'checked\\', \\'checked\\'); editorWindow.editAlert();\" />");
    editor->page()->mainFrame()->evaluateJavaScript(
            script_start + todo + script_end);
    editor->setFocus();
    microFocusChanged();
}



// The font size button was pressed
void NBrowserWindow::fontSizeSelected(int index) {
    int size = buttonBar->fontSizes->itemData(index).toInt();

    QString text = editor->selectedText();
    if (text.trimmed() == "")
        return;

    QString newText = "<span style=\"font-size:" +QString::number(size) +"pt;\">"+text+"</span>";
    QString script = QString("document.execCommand('insertHtml', false, '"+newText+"');");
    QLOG_DEBUG() << script;
    editor->page()->mainFrame()->evaluateJavaScript(script);
    editor->setFocus();
    microFocusChanged();
}



// The font name list was selected
void NBrowserWindow::fontNameSelected(int index) {
    QString font = buttonBar->fontNames->itemData(index).toString();
    buttonBar->loadFontSizeComboBox(font);
    this->editor->page()->mainFrame()->evaluateJavaScript(
            "document.execCommand('fontName', false, '"+font+"');");
    editor->setFocus();
    microFocusChanged();
}



// The font highlight color was pressed
void NBrowserWindow::fontHighlightClicked() {
    QColor *color = buttonBar->highlightColorMenuWidget->getColor();
    if (color->isValid()) {
        this->editor->page()->mainFrame()->evaluateJavaScript(
                "document.execCommand('backColor', false, '"+color->name()+"');");
        editor->setFocus();
        microFocusChanged();
    }
}



// The font color was pressed
void NBrowserWindow::fontColorClicked() {
    QColor *color = buttonBar->fontColorMenuWidget->getColor();
    if (color->isValid()) {
        this->editor->page()->mainFrame()->evaluateJavaScript(
                "document.execCommand('foreColor', false, '"+color->name()+"');");
        editor->setFocus();
        microFocusChanged();
    }
}


void NBrowserWindow::insertLinkButtonPressed() {
    QString text = editor->selectedText();
    if (text.trimmed() == "" && currentHyperlink.trimmed() == "")
        return;

    InsertLinkDialog dialog(insertHyperlink);
    if (currentHyperlink != NULL && currentHyperlink != "") {
        dialog.setUrl(currentHyperlink);
    }
    dialog.exec();
    if (!dialog.okButtonPressed()) {
        return;
    }

    // Take care of inserting new links
    if (insertHyperlink) {
        QString selectedText = editor->selectedText();
        if (dialog.getUrl().trimmed() == "")
            return;
        QString dUrl = dialog.getUrl().trimmed().replace("'", "\\'");
        QString url = QString("<a href=\"") +dUrl
                +QString("\" title=") +dUrl
                +QString(" >") +selectedText +QString("</a>");
        QString script = QString("document.execCommand('insertHtml', false, '")+url+QString("');");
        editor->page()->mainFrame()->evaluateJavaScript(script);
        return;
    }

    QString x = dialog.getUrl();
    // Edit existing links
    QString js =  "function getCursorPos() {"
            "var cursorPos;"
            "if (window.getSelection) {"
            "   var selObj = window.getSelection();"
            "   var selRange = selObj.getRangeAt(0);"
            "   var workingNode = window.getSelection().anchorNode.parentNode;"
            "   while(workingNode != null) { "
            "      if (workingNode.nodeName.toLowerCase()=='a') workingNode.setAttribute('href','";
    js = js + dialog.getUrl() +QString("');")
            +QString("      workingNode = workingNode.parentNode;")
            +QString("   }")
            +QString("}")
            +QString("} getCursorPos();");
    editor->page()->mainFrame()->evaluateJavaScript(js);

    if (dialog.getUrl().trimmed() != "" ) {
        contentChanged();
        return;
    }

    // Remove URL
    js = QString( "function getCursorPos() {")
            + QString("var cursorPos;")
            + QString("if (window.getSelection) {")
            + QString("   var selObj = window.getSelection();")
            + QString("   var selRange = selObj.getRangeAt(0);")
            + QString("   var workingNode = window.getSelection().anchorNode.parentNode;")
            + QString("   while(workingNode != null) { ")
            + QString("      if (workingNode.nodeName.toLowerCase()=='a') { ")
            + QString("         workingNode.removeAttribute('href');")
            + QString("         workingNode.removeAttribute('title');")
            + QString("         var text = document.createTextNode(workingNode.innerText);")
            + QString("         workingNode.parentNode.insertBefore(text, workingNode);")
            + QString("         workingNode.parentNode.removeChild(workingNode);")
            + QString("      }")
            + QString("      workingNode = workingNode.parentNode;")
            + QString("   }")
            + QString("}")
            + QString("} getCursorPos();");
        editor->page()->mainFrame()->evaluateJavaScript(js);

        contentChanged();
}


void NBrowserWindow::insertQuickLinkButtonPressed() {
    QString text = editor->selectedText();
    if (text.trimmed() == "")
        return;

    NoteTable ntable(global.db);
    QList<qint32> lids;
    if (!ntable.findNotesByTitle(lids, text))
        if (!ntable.findNotesByTitle(lids, text.trimmed()+"%"))
            if (!ntable.findNotesByNotebook(lids, "%"+text.trimmed()+"%"))
                return;
    Note n;

    // If we have a good return, then we can paste the link, otherwise we fall out
    // to a normal paste.
    if (ntable.get(n, lids[0],false,false)) {
        UserTable utable(global.db);
        User user;
        utable.getUser(user);

        QString href = "evernote:///view/" + QString::number(user.id) + QString("/") +
                QString::fromStdString(user.shardId) +QString("/") +
                QString::fromStdString(n.guid) +QString("/") +
                QString::fromStdString(n.guid);

        QString url = QString("<a href=\"") +href
                +QString("\" title=") +text
                +QString("\">") +text +QString("</a>");
        QString script = QString("document.execCommand('insertHtml', false, '")+url+QString("');");
        editor->page()->mainFrame()->evaluateJavaScript(script);
        return;
    }
}


void NBrowserWindow::insertLatexButtonPressed() {
    this->editLatex("");
}


void NBrowserWindow::encryptButtonPressed() {
        EnCrypt encrypt;

    QString text = editor->selectedText();
    if (text.trimmed() == "")
        return;
    text = text.replace("\n", "<br/>");

    EnCryptDialog dialog;
    dialog.exec();
    if (!dialog.okPressed()) {
        return;
    }
}


void NBrowserWindow::insertTableButtonPressed() {
    TableDialog dialog(this);
    dialog.exec();
    if (!dialog.isOkPressed()) {
        return;
    }

    int cols = dialog.getCols();
    int rows = dialog.getRows();
    int width = dialog.getWidth();
    bool percent = dialog.isPercent();

    QString newHTML = QString("<table border=\"1\" width=\"") +QString::number(width);
    if (percent)
        newHTML = newHTML +"%";
    newHTML = newHTML + "\"><tbody>";

    for (int i=0; i<rows; i++) {
        newHTML = newHTML +"<tr>";
        for (int j=0; j<cols; j++) {
            newHTML = newHTML +"<td>&nbsp;</td>";
        }
        newHTML = newHTML +"</tr>";
    }
    newHTML = newHTML+"</tbody></table>";

    QString script = "document.execCommand('insertHtml', false, '"+newHTML+"');";
    editor->page()->mainFrame()->evaluateJavaScript(script);
    contentChanged();
}

void NBrowserWindow::insertTableRowButtonPressed() {
    QString js ="function insertTableRow() {"
        "   var selObj = window.getSelection();"
        "   var selRange = selObj.getRangeAt(0);"
        "   var workingNode = window.getSelection().anchorNode.parentNode;"
        "   var cellCount = 0;"
        "   while(workingNode != null) { "
        "      if (workingNode.nodeName.toLowerCase()=='tr') {"
        "           row = document.createElement('TR');"
        "           var nodes = workingNode.getElementsByTagName('td');"
        "           for (j=0; j<nodes.length; j=j+1) {"
        "              cell = document.createElement('TD');"
        "              cell.innerHTML='&nbsp;';"
        "              row.appendChild(cell);"
        "           }"
        "           workingNode.parentNode.insertBefore(row,workingNode.nextSibling);"
        "           return;"
        "      }"
        "      workingNode = workingNode.parentNode;"
        "   }"
        "} insertTableRow();";
    editor->page()->mainFrame()->evaluateJavaScript(js);
    contentChanged();
}


void NBrowserWindow::insertTableColumnButtonPressed() {
    QString js = "function insertTableColumn() {"
            "   var selObj = window.getSelection();"
            "   var selRange = selObj.getRangeAt(0);"
            "   var workingNode = window.getSelection().anchorNode.parentNode;"
            "   var current = 0;"
            "   while (workingNode.nodeName.toLowerCase() != 'table' && workingNode != null) {"
            "       if (workingNode.nodeName.toLowerCase() == 'td') {"
            "          var td = workingNode;"
            "          while (td.previousSibling != null) { "
            "             current = current+1; td = td.previousSibling;"
            "          }"
            "       }"
            "       workingNode = workingNode.parentNode; "
            "   }"
            "   if (workingNode == null) return;"
            "   for (var i=0; i<workingNode.rows.length; i++) { "
            "      var cell = workingNode.rows[i].insertCell(current+1); "
            "      cell.innerHTML = '&nbsp'; "
            "   }"
            "} insertTableColumn();";
        editor->page()->mainFrame()->evaluateJavaScript(js);
        contentChanged();
}


void NBrowserWindow::deleteTableRowButtonPressed() {
    QString js = "function deleteTableRow() {"
        "   var selObj = window.getSelection();"
        "   var selRange = selObj.getRangeAt(0);"
        "   var workingNode = window.getSelection().anchorNode.parentNode;"
        "   var cellCount = 0;"
        "   while(workingNode != null) { "
        "      if (workingNode.nodeName.toLowerCase()=='tr') {"
        "           workingNode.parentNode.removeChild(workingNode);"
        "           return;"
        "      }"
        "      workingNode = workingNode.parentNode;"
        "   }"
        "} deleteTableRow();";
    editor->page()->mainFrame()->evaluateJavaScript(js);
    contentChanged();
}


void NBrowserWindow::deleteTableColumnButtonPressed() {
    QString js = "function deleteTableColumn() {"
            "   var selObj = window.getSelection();"
            "   var selRange = selObj.getRangeAt(0);"
            "   var workingNode = window.getSelection().anchorNode.parentNode;"
            "   var current = 0;"
            "   while (workingNode.nodeName.toLowerCase() != 'table' && workingNode != null) {"
            "       if (workingNode.nodeName.toLowerCase() == 'td') {"
            "          var td = workingNode;"
            "          while (td.previousSibling != null) { "
            "             current = current+1; td = td.previousSibling;"
            "          }"
            "       }"
            "       workingNode = workingNode.parentNode; "
            "   }"
            "   if (workingNode == null) return;"
            "   for (var i=0; i<workingNode.rows.length; i++) { "
            "      workingNode.rows[i].deleteCell(current); "
            "   }"
            "} deleteTableColumn();";
        editor->page()->mainFrame()->evaluateJavaScript(js);
        contentChanged();
}

void NBrowserWindow::rotateImageLeftButtonPressed() {
    rotateImage(-90.0);
}




void NBrowserWindow::rotateImageRightButtonPressed() {
    rotateImage(90.0);
}


void NBrowserWindow::rotateImage(qreal degrees) {

    // rotate the image
    QWebSettings::setMaximumPagesInCache(0);
    QWebSettings::setObjectCacheCapacities(0, 0, 0);
    QImage image(global.fileManager.getDbaDirPath() +selectedFileName);
    QMatrix matrix;
    matrix.rotate( degrees );
    image = image.transformed(matrix);
    image.save(global.fileManager.getDbaDirPath() +selectedFileName);
    editor->setHtml(editor->page()->mainFrame()->toHtml());

    // Now, we need to update the note's MD5
    QFile f(global.fileManager.getDbaDirPath() +selectedFileName);
    f.open(QIODevice::ReadOnly);
    QByteArray filedata = f.readAll();
    QCryptographicHash hash(QCryptographicHash::Md5);
    QByteArray b = hash.hash(filedata, QCryptographicHash::Md5);
    updateImageHash(b);

    // Reload the web page
    editor->triggerPageAction(QWebPage::ReloadAndBypassCache);
    contentChanged();
}


void NBrowserWindow::updateImageHash(QByteArray newhash) {
    QString content = editor->page()->mainFrame()->toHtml();
    int pos = content.indexOf("<img ");
    for (; pos>0; pos=content.indexOf("<img ", pos+1) ) {
        int endPos = content.indexOf(">", pos);
        QString section = content.mid(pos, endPos-pos);
        if (section.indexOf("lid=\"" +QString::number(selectedFileLid) + "\"") > 0) {
            ResourceTable rtable(global.db);
            QString oldhash = section.mid(section.indexOf("hash=\"")+6);
            oldhash = oldhash.mid(0,oldhash.indexOf("\""));
            section.replace(oldhash, newhash.toHex());
            QString newcontent = content.mid(0,pos) +section +content.mid(endPos);
            QByteArray c;
            c.append(newcontent);
            editor->page()->mainFrame()->setContent(c);
            rtable.updateResourceHash(selectedFileLid, newhash);
            return;
        }
    }
}

void NBrowserWindow::imageContextMenu(QString l, QString f) {
    editor->downloadAttachmentAction()->setEnabled(true);
    editor->rotateImageRightAction->setEnabled(true);
    editor->rotateImageLeftAction->setEnabled(true);
    editor->openAction->setEnabled(true);
    editor->downloadImageAction()->setEnabled(true);
    selectedFileName = f;
    selectedFileLid = l.toInt();
}


void NBrowserWindow::attachFile() {
    QFileDialog fileDialog;
    fileDialog.setDirectory(QDir::homePath());
    fileDialog.setFileMode(QFileDialog::ExistingFile);
    connect(&fileDialog, SIGNAL(fileSelected(QString)), this, SLOT(attachFileSelected(QString)));
    int rc = fileDialog.exec();
}



//****************************************************************
//* MicroFocus changed
//****************************************************************
 void NBrowserWindow::microFocusChanged() {
     buttonBar->boldButtonWidget->setDown(false);
     buttonBar->italicButtonWidget->setDown(false);
     buttonBar->underlineButtonWidget->setDown(false);
     editor->openAction->setEnabled(false);
     editor->downloadAttachmentAction()->setEnabled(false);
     editor->rotateImageLeftAction->setEnabled(false);
     editor->rotateImageRightAction->setEnabled(false);
     editor->insertTableAction->setEnabled(true);
     editor->insertTableColumnAction->setEnabled(false);
     editor->insertTableRowAction->setEnabled(false);
     editor->deleteTableRowAction->setEnabled(false);
     editor->deleteTableColumnAction->setEnabled(false);
     editor->insertLinkAction->setText(tr("Insert Link"));
     editor->insertQuickLinkAction->setEnabled(true);
     editor->rotateImageRightAction->setEnabled(false);
     editor->rotateImageLeftAction->setEnabled(false);

     insertHyperlink = true;
     currentHyperlink ="";
     insideList = false;
     insideTable = false;
     insideEncryption = false;
     forceTextPaste = false;

    QString js = QString("function getCursorPos() {")
        +QString("var cursorPos;")
        +QString("if (window.getSelection) {")
        +QString("   var selObj = window.getSelection();")
        +QString("   var selRange = selObj.getRangeAt(0);")
        +QString("   var workingNode = window.getSelection().anchorNode.parentNode;")
        +QString("   while(workingNode != null) { ")
        //+QString("      window.browserWindow.printNodeName(workingNode.nodeName);")
        +QString("      if (workingNode.nodeName=='TABLE') { if (workingNode.getAttribute('class').toLowerCase() == 'en-crypt-temp') window.browserWindow.insideEncryptionArea(); }")
        +QString("      if (workingNode.nodeName=='B') window.browserWindow.boldActive();")
        +QString("      if (workingNode.nodeName=='I') window.browserWindow.italicsActive();")
        +QString("      if (workingNode.nodeName=='U') window.browserWindow.underlineActive();")
        +QString("      if (workingNode.nodeName=='UL') window.browserWindow.setInsideList();")
        +QString("      if (workingNode.nodeName=='OL') window.browserWindow.setInsideList();")
        +QString("      if (workingNode.nodeName=='LI') window.browserWindow.setInsideList();")
        +QString("      if (workingNode.nodeName=='TBODY') window.browserWindow.setInsideTable();")
        +QString("      if (workingNode.nodeName=='A') {for(var x = 0; x < workingNode.attributes.length; x++ ) {if (workingNode.attributes[x].nodeName.toLowerCase() == 'href') window.browserWindow.setInsideLink(workingNode.attributes[x].nodeValue);}}")
        +QString("      if (workingNode.nodeName=='SPAN') {")
        +QString("         if (workingNode.getAttribute('style') == 'text-decoration: underline;') window.browserWindow.underlineActive();")
        +QString("      }")
        +QString("      workingNode = workingNode.parentNode;")
        +QString("   }")
        +QString("}")
        +QString("}  getCursorPos();");
    editor->page()->mainFrame()->evaluateJavaScript(js);
}



 // Tab button pressed
 void NBrowserWindow::tabPressed() {
     if (insideEncryption)
         return;
     if (!insideList && !insideTable) {
         QString script_start =  "document.execCommand('insertHtml', false, '&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;');";
         editor->page()->mainFrame()->evaluateJavaScript(script_start);
         return;
     }
     if (insideList) {
         shiftRightButtonPressed();
     }
     if (insideTable) {
         QString js =  "function getCursorPosition() { "
                 "   var selObj = window.getSelection();"
                 "   var selRange = selObj.getRangeAt(0);"
                 "   var workingNode = window.getSelection().anchorNode;"
                 "   var rowCount = 0;"
                 "   var colCount = 0;"
                 "   while(workingNode != null && workingNode.nodeName.toLowerCase() != 'table') { "
                 "      if (workingNode.nodeName.toLowerCase()=='tr') {"
                 "         rowCount = rowCount+1;"
                 "      }"
                 "      if (workingNode.nodeName.toLowerCase() == 'td') {"
                 "         colCount = colCount+1;"
                 "      }"
                 "      if (workingNode.previousSibling != null)"
                 "          workingNode = workingNode.previousSibling;"
                 "      else "
                 "           workingNode = workingNode.parentNode;"
                 "   }"
                 "   var nodes = workingNode.getElementsByTagName('tr');"
                 "   var tableRows = nodes.length;"
                 "   nodes = nodes[0].getElementsByTagName('td');"
                 "   var tableColumns = nodes.length;"
                 "   window.jambi.setTableCursorPositionTab(rowCount, colCount, tableRows, tableColumns);"
                 "} getCursorPosition();";
         editor->page()->mainFrame()->evaluateJavaScript(js);
     }

 }


 // Backtab pressed.
 void NBrowserWindow::backtabPressed() {
     if (insideEncryption)
         return;
     if (insideList)
         shiftLeftButtonPressed();
     if (insideTable) {
         QString js = "function getCursorPosition() { "
                 "   var selObj = window.getSelection();"
                 "   var selRange = selObj.getRangeAt(0);"
                 "   var workingNode = window.getSelection().anchorNode;"
                 "   var rowCount = 0;"
                 "   var colCount = 0;"
                 "   while(workingNode != null && workingNode.nodeName.toLowerCase() != 'table') { "
                 "      if (workingNode.nodeName.toLowerCase()=='tr') {"
                 "         rowCount = rowCount+1;"
                 "      }"
                 "      if (workingNode.nodeName.toLowerCase() == 'td') {"
                 "         colCount = colCount+1;"
                 "      }"
                 "      if (workingNode.previousSibling != null)"
                 "          workingNode = workingNode.previousSibling;"
                 "      else "
                 "           workingNode = workingNode.parentNode;"
                 "   }"
                 "   var nodes = workingNode.getElementsByTagName('tr');"
                 "   var tableRows = nodes.length;"
                 "   nodes = nodes[0].getElementsByTagName('td');"
                 "   var tableColumns = nodes.length;"
                 "   window.jambi.setTableCursorPositionBackTab(rowCount, colCount, tableRows, tableColumns);"
                 "} getCursorPosition();";
         editor->page()->mainFrame()->evaluateJavaScript(js);
     }
 }


// Set the backgroud color of a note
 void NBrowserWindow::setBackgroundColor(QString value) {
     QString js = QString("function changeBackground(color) {")
         +QString("document.body.style.background = color;")
         +QString("}")
         +QString("changeBackground('" +value+"');");
     editor->page()->mainFrame()->evaluateJavaScript(js);
     editor->setFocus();
     microFocusChanged();
 }


 // The user clicked a link in the note
 void NBrowserWindow::linkClicked(const QUrl url) {
     if (url.toString().startsWith("latex://", Qt::CaseInsensitive)) {
         editLatex(url.toString().mid(8));
         return;
     }
     if (url.toString().startsWith("evernote:/view/", Qt::CaseInsensitive) ||
             url.toString().startsWith("evernote:///view/", Qt::CaseInsensitive)) {
         QStringList tokens;
         if (url.toString().startsWith("evernote:/view/", Qt::CaseInsensitive))
            tokens = url.toString().replace("evernote:/view/", "").split("/", QString::SkipEmptyParts);
         else
            tokens = url.toString().replace("evernote:///view/", "").split("/", QString::SkipEmptyParts);
         QString oguid =tokens[2];
         QString eguid = tokens[3];
         NoteTable ntable(global.db);
         qint32 newlid = ntable.getLid(eguid);
         if (newlid <= 0)
             newlid = ntable.getLid(oguid);
         if (newlid <= 0)
             return;


         // Setup a new filter
         FilterCriteria *criteria = new FilterCriteria();
         global.filterCriteria[global.filterPosition]->duplicate(*criteria);
         criteria->unsetSelectedNotes();
         criteria->unsetLid();
         criteria->setLid(newlid);
         global.appendFilter(criteria);
         global.filterPosition++;
         emit(evernoteLinkClicked(newlid, false));
         return;
     }
     if (url.toString().startsWith("nnres:", Qt::CaseInsensitive)) {
         if (url.toString().endsWith("/vnd.evernote.ink")) {
             QMessageBox::information(this, tr("Unable Open"), QString(tr("This is an ink note.\nInk notes are not supported since Evernote has not\n published any specifications on them\nand I'm too lazy to figure them out by myself.")));
             return;
         }
         QString fullName = url.toString().mid(6);
         int index = fullName.lastIndexOf(".");
         QString guid = "";
         QString type = "";
         if (index >-1) {
             type = fullName.mid(index);
             guid = fullName.mid(0,index).replace(global.fileManager.getDbaDirPath(),"");
         } else
             return;
         QString fileUrl = global.fileManager.getDbaDirPath()+guid +type;
         global.resourceWatcher.addPath(fileUrl);
         QDesktopServices::openUrl(fileUrl);
         return;
     }
     QDesktopServices::openUrl(url);
 }



 // show/hide view source window
void NBrowserWindow::showSource(bool value) {
     setSource();
     sourceEdit->setVisible(value);
 }



// Toggle the show source button
void NBrowserWindow::toggleSource() {
    if (sourceEdit->isVisible())
        showSource(false);
    else
        showSource(true);
}



// Clear out the window's contents
void NBrowserWindow::clear() {
    sourceEdit->blockSignals(true);
    editor->blockSignals(true);
    sourceEdit->setPlainText("");
    editor->setContent("<html><body></body></html>");
    sourceEdit->setReadOnly(true);
    editor->page()->setContentEditable(false);
    lid = -1;
    editor->blockSignals(false);
    sourceEdit->blockSignals(false);
}



// Set the source for the "show source" button
void NBrowserWindow::setSource() {
    QString text = editor->editorPage->mainFrame()->toHtml();
    sourceEdit->blockSignals(true);
    int body = text.indexOf("<body", Qt::CaseInsensitive);
    if (body > 0) {
        body = text.indexOf(">",body);
        if (body > 0) {
            sourceEditHeader =text.mid(0, body+1);
            text = text.mid(body+1);
        }
    }
    text = text.replace("</body></html>", "");
    sourceEdit->setPlainText(text);
 //   sourceEdit->setReadOnly(true);
    sourceEdit->setReadOnly(!editor->page()->isContentEditable());
    sourceEdit->blockSignals(false);
}



// Expose the programs to the javascript process
void NBrowserWindow::exposeToJavascript() {
    editor->page()->mainFrame()->addToJavaScriptWindowObject("browserWindow", this);
}



// If we are within bold text, set the bold button active
void NBrowserWindow::boldActive() {
    buttonBar->boldButtonWidget->setDown(true);
}



// If we are within italics text, make the text button active
void NBrowserWindow::italicsActive() {
   buttonBar->italicButtonWidget->setDown(true);
}



// If we are within encrypted text, make sure we force a paste text
void NBrowserWindow::insideEncryptionArea() {
    insideEncryption = true;
    forceTextPaste = true;
}



// If we are within underlined text, make the button active
void NBrowserWindow::underlineActive() {
    buttonBar->underlineButtonWidget->setDown(true);
}



// Set true if we are within some type of list
void NBrowserWindow::setInsideList() {
    insideList = true;
}



// If we are within a table, set the menu options active
void NBrowserWindow::setInsideTable() {
    editor->insertTableAction->setEnabled(false);
    editor->insertTableRowAction->setEnabled(true);
    editor->insertTableColumnAction->setEnabled(true);
    editor->deleteTableRowAction->setEnabled(true);
    editor->deleteTableColumnAction->setEnabled(true);
    editor->encryptAction->setEnabled(false);
    insideTable = true;
}


// Set if we are within a link
void NBrowserWindow::setInsideLink(QString link) {
    currentHyperlink = link;
    editor->insertLinkAction->setText(tr("Edit Link"));
    currentHyperlink = link;
    insertHyperlink = false;
}




// Edit a latex formula
void NBrowserWindow::editLatex(QString guid) {
    QString text = editor->selectedText();
    QString oldFormula = "";
    if (text.trimmed() == "\n" || text.trimmed() == "") {
        InsertLatexDialog dialog;
        if (guid.trimmed() != "") {
            Resource r;
            ResourceTable resTable(global.db);
            resTable.get(r, guid.toInt());
            if (r.__isset.attributes && r.attributes.__isset.sourceURL) {
                QString formula = QString::fromStdString(r.attributes.sourceURL);
                formula = formula.replace("http://latex.codecogs.com/gif.latex?", "");
                oldFormula = formula;
                dialog.setFormula(formula);
            }
        }
        dialog.exec();
        if (!dialog.okPressed()) {
            return;
        }
        text = dialog.getFormula().trimmed();
    }

    ConfigStore cs(global.db);
    qint32 newlid = cs.incrementLidCounter();
    Resource r;
    NoteTable ntable(global.db);
    ResourceTable rtable(global.db);
    QString outfile = global.fileManager.getDbaDirPath() + QString::number(newlid) +QString(".gif");

    // Run it through "mimetex" to create the gif
        text = text.replace("'", "\\'");
    QProcess latexProcess;
    QStringList args;
    args.append("-e");
    args.append(outfile);
    args.append(text);
    QString formula = "mimetex -e "+outfile +" '" +text +"'";
    QLOG_DEBUG() << "Formula:" << formula;
    //latexProcess.start(formula, QIODevice::ReadWrite|QIODevice::Unbuffered);
    latexProcess.start("mimetex", args, QIODevice::ReadWrite|QIODevice::Unbuffered);

    QLOG_DEBUG() << "Starting mimetex " << latexProcess.waitForStarted();
    QLOG_DEBUG() << "Stopping mimetex " << latexProcess.waitForFinished() << " Return Code: " << latexProcess.state();
    QLOG_DEBUG() << "mimetex Errors:" << latexProcess.readAllStandardError();
    QLOG_DEBUG() << "mimetex Output:" << latexProcess.readAllStandardOutput();

    // Now, check if the file exists.  If it does, we continue to create the resource
    QFile f(outfile);
    if (!f.exists()) {
        QMessageBox msgBox;
        msgBox.setText(tr("Unable to create LaTeX image"));
        msgBox.setInformativeText(tr("Unable to create LaTeX image.  Are you sure mimetex is installed?"));
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setIcon(QMessageBox::Critical);
        msgBox.setDefaultButton(QMessageBox::Ok);
        msgBox.exec();
        return;
    }
    f.open(QIODevice::ReadOnly);
    QByteArray data = f.readAll();
    f.close();
    f.open(QIODevice::ReadOnly);
    r.data.body.resize(data.size());
    char *dataptr = (char *)&r.data.body.data()[0];
    f.read(dataptr, r.data.body.size());
    f.close();

    r.guid = QString::number(newlid).toStdString();
    r.__isset.guid = true;
    r.noteGuid = ntable.getGuid(lid).toStdString();
    r.__isset.guid = true;

    QCryptographicHash md5hash(QCryptographicHash::Md5);
    QByteArray hash = md5hash.hash(data, QCryptographicHash::Md5);

    r.__isset.noteGuid = true;
    r.mime = "image/gif";
    r.__isset.mime = true;
    r.active = true;
    r.__isset.active = true;
    r.updateSequenceNum = 0;
    r.__isset.updateSequenceNum = true;
    r.width = 0;
    r.__isset.width = 0;
    r.height = 0;
    r.__isset.height = 0;
    r.duration = 0;
    r.__isset.duration = 0;

    r.__isset.data = true;
    r.data.__isset.body = true;
    r.data.bodyHash.append(hash.data(), hash.size());
    r.data.__isset.bodyHash = true;
    r.data.size = data.size();
    r.data.__isset.size = true;

    r.__isset.attributes = true;
    r.attributes.attachment = false;
    r.attributes.__isset.attachment = true;
    r.attributes.sourceURL = "http://latex.codecogs.com/gif.latex?" +text.toStdString();
    r.attributes.__isset.sourceURL = true;

    rtable.add(newlid, r, true, lid);

    // do the actual insert into the note

    QString buffer;
    buffer.append("<a onmouseover=\"cursor:&apos;hand&apos; title=\"");
    buffer.append(text);
    buffer.append("\" href=\"latex://");
    buffer.append(QString::number(newlid));
    buffer.append("\">");
    buffer.append("<img src=\"file://");
    buffer.append(outfile);
    buffer.append("\" type=\"image/gif\" hash=\"");
    buffer.append(hash.toHex());
    buffer.append("\" onContextMenu=\"window.browser.imageContextMenu(&apos;");
    buffer.append(QString::number(newlid));
    buffer.append("&apos;, &apos;");
    buffer.append(outfile);
    buffer.append("&apos;);\" ");
    buffer.append(" en-tag=\"en-latex\" lid=\"");
    buffer.append(QString::number(newlid));
    buffer.append("\"></a>");

    // If this is a new formula, we insert it, otherwise we replace the old one.
    if (oldFormula == "") {
        QString script_start = "document.execCommand('insertHTML', false, '";
        QString script_end = "');";

        editor->page()->mainFrame()->evaluateJavaScript(
                script_start + buffer + script_end);
    } else {
        QString oldHtml = editor->page()->mainFrame()->toHtml();
        int startPos = oldHtml.indexOf("<a");
        int endPos = oldHtml.indexOf("</a>", startPos);
        while (startPos > 0) {
            if (endPos > 0) {
                QString slice = oldHtml.mid(startPos, endPos-startPos+4);
                if (slice.indexOf("lid=\""+guid+"\"") && slice.indexOf("en-latex")) {
                    oldHtml.replace(slice, buffer);
                }
                startPos = oldHtml.indexOf("<a", endPos);
                editor->page()->mainFrame()->setHtml(oldHtml);
                editor->reload();
                contentChanged();
            }
        }
    }
}


// Set the focus to the note title
void NBrowserWindow::focusTitle() {
    this->noteTitle.setFocus();
}


// Set the focus to the note
void NBrowserWindow::focusNote() {
    this->editor->setFocus();
}


// Insert the date/time into a note
void NBrowserWindow::insertDatetime() {
    QDateTime dt = QDateTime::currentDateTime();
    QLocale locale;
    QString dts = dt.toString(locale.dateTimeFormat(QLocale::ShortFormat));

    editor->page()->mainFrame()->evaluateJavaScript(
        "document.execCommand('insertHtml', false, '"+dts+"');");
    editor->setFocus();
}



// Insert an image into the editor
void NBrowserWindow::insertImage(const QMimeData *mime) {

    // Get the image from the clipboard and save it into a QByteArray
    // that can be saved
    QImage img = qvariant_cast<QImage>(mime->imageData());
    QByteArray imageBa;
    QBuffer b(&imageBa);
    b.open(QIODevice::WriteOnly);
    img.save(&b, "JPG");

    QString script_start = "document.execCommand('insertHTML', false, '";
    QString script_end = "');";

    Resource newRes;
    qint32 rlid = createResource(newRes, 0, imageBa, "image/jpeg", false, "");
    if (rlid <= 0)
        return;

    // The resource is done, now we need to add it to the
    // note body
    QString g =  QString::number(rlid)+QString(".jpg");
    QString path = global.fileManager.getDbaDirPath() + g;

    // do the actual insert into the note
    QString buffer;
    QByteArray hash(newRes.data.bodyHash.c_str(), newRes.data.bodyHash.size());
    buffer.append("<img src=\"file://");
    buffer.append(path);
    buffer.append("\" type=\"image/jpeg\" hash=\"");
    //buffer.append(QString::fromStdString(newRes.data.bodyHash));
    buffer.append(hash.toHex());
    buffer.append("\" onContextMenu=\"window.browser.imageContextMenu(&apos;");
    buffer.append(QString::number(rlid));
    buffer.append("&apos;, &apos;");
    buffer.append(g);
    buffer.append("&apos;);\" ");
    buffer.append(" en-tag=\"en-media\" style=\"cursor: default;\" lid=\"");
    buffer.append(QString::number(rlid));
    buffer.append("\">");

    // Insert the actual note
    editor->page()->mainFrame()->evaluateJavaScript(
            script_start + buffer + script_end);

    return;
}


// Create  a new resource and add it to the database
qint32 NBrowserWindow::createResource(Resource &r, int sequence, QByteArray data,  QString mime, bool attachment, QString filename) {
    ConfigStore cs(global.db);
    qint32 rlid = cs.incrementLidCounter();

    QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Md5);

    QString guid =  QString::number(rlid);
    NoteTable noteTable(global.db);
    r.guid = guid.toStdString();
    r.__isset.guid = true;
    r.noteGuid = noteTable.getGuid(lid).toStdString();
    if (r.noteGuid == "")
        return 0;
    r.__isset.noteGuid = true;
    r.mime = mime.toStdString();
    r.__isset.mime = true;
    r.active = true;
    r.__isset.active = true;
    r.updateSequenceNum = sequence;
    r.__isset.updateSequenceNum = true;
    r.width = 0;
    r.__isset.width = 0;
    r.height = 0;
    r.__isset.height = 0;
    r.duration = 0;
    r.__isset.duration = 0;
    if (filename != "") {
        r.attributes.fileName = filename.toStdString();
        r.__isset.attributes = true;
        r.attributes.__isset.fileName = true;
    }

    Data *d = &r.data;
    r.__isset.data = true;
    d->body.clear();
    d->body.append(data.data(), data.size());
    d->__isset.body = true;
    d->bodyHash.append(hash.data(), hash.size());
    d->__isset.bodyHash = true;
    d->size = data.size();
    d->__isset.size = true;

    ResourceAttributes *a = &r.attributes;
    a->attachment = attachment;
    a->__isset.attachment = true;

    ResourceTable resourceTable(global.db);
    resourceTable.add(rlid, r, true, lid);

    return rlid;
}


// Print the contents of a note.  Basically it loops through the
// note and repaces the <object> tags with <img> tags.  The plugin
// object should be creating temporary images for the print.
void NBrowserWindow::printNote(QPrinter *printer) {
    NWebView *tempEditor = new NWebView(this);
    QString contents = editor->editorPage->mainFrame()->toHtml();

    // Start removing object tags
    int pos = contents.indexOf("<object");
    while (pos>=0) {
        int endPos = contents.indexOf(">", pos);
        QString lidString = contents.mid(contents.indexOf("lid=", pos)+5);
        lidString = lidString.mid(0,lidString.indexOf("\" "));
        contents = contents.mid(0,pos) + "<img src=\"file://" +
                global.fileManager.getTmpDirPath() + lidString +
                QString("-print.png\" width=\"10%\" height=\"10%\"></img>")+contents.mid(endPos+1);

        pos = contents.indexOf("<object", endPos);
    }

    // Hack to do a synchronous load of a web page.  Basically loop until the
    // load is finished.  If we don't do this the web page might try to print
    // before any images are rendered.
    QEventLoop loop;
    QObject::connect(tempEditor, SIGNAL(loadFinished(bool)), &loop, SLOT(quit()));

//    tempEditor->setContent(contents.toUtf8(),  "application/xhtml+xml");
    tempEditor->setContent(contents.toUtf8());
    loop.exec();

    // Do the actual print
    tempEditor->print(printer);

    // Cleanup
    QObject::disconnect(&loop, SLOT(quit()));
    delete tempEditor;
}



void NBrowserWindow::noteSourceUpdated() {
    QByteArray ba;
    QString source = sourceEdit->toPlainText();
   //source = Qt::escape(source);
    ba.append(sourceEditHeader);
    ba.append(source);
    ba.append("</body></html>");
//    editor->setContent(ba,  "application/xhtml+xml");
    editor->setContent(ba);
    this->editor->isDirty = true;
}

// Update a resource's hash if it was edited somewhere else
void NBrowserWindow::updateResourceHash(qint32 noteLid, QByteArray oldHash, QByteArray newHash) {
    if (noteLid != lid)
        return;

    QString content = editor->editorPage->mainFrame()->documentElement().toOuterXml();

    // Start going through & looking for the old hash
    int pos = content.indexOf("<body");
    int endPos;
    int hashPos = -1;
    QString hashString = "hash=\"" +oldHash.toHex() +"\"";
    while (pos>0) {
        endPos = content.indexOf(">", pos);  // Find the matching end of the tag
        hashPos = content.indexOf(hashString, pos);
        if (hashPos < endPos && hashPos > 0) {  // If we found the hash, begin the update
            QString startString = content.mid(0, hashPos);
            QString endString = content.mid(hashPos+hashString.length());
            QString newContent = startString + "hash=\"" +newHash.toHex() +"\"" +endString;
            QByteArray byteArray;
            byteArray.append(newContent);
            editor->setContent(byteArray);
            noteUpdated(lid);
            return;
        } else {
            pos = content.indexOf("<", pos+1);
        }
    }


}



void NBrowserWindow::attachFileSelected(QString filename) {
    // Read in the file
    QFile file(filename);
    file.open(QIODevice::ReadOnly);
    QByteArray ba = file.readAll();
    file.close();

    QString script_start = "document.execCommand('insertHTML', false, '";
    QString script_end = "');";

    MimeReference mimeRef;
    QString extension = filename;
    int endPos = filename.lastIndexOf(".");
    if (endPos>0)
        extension = extension.mid(endPos);
    QString mime =  mimeRef.getMimeFromExtension(extension);
    Resource newRes;
    bool attachment = true;
    if (mime == "application/pdf" || mime.startsWith("image/"))
        attachment = false;
    qint32 rlid = createResource(newRes, 0, ba, mime, attachment, QFileInfo(filename).fileName());
    QByteArray hash;
    hash.append(newRes.data.bodyHash.data(), newRes.data.bodyHash.size());
    if (rlid <= 0)
        return;

    // If we have an image, then insert it.
    if (mime.startsWith("image", Qt::CaseInsensitive)) {

        // The resource is done, now we need to add it to the
        // note body
        QString g =  QString::number(rlid)+extension;
        QString path = global.fileManager.getDbaDirPath() + g;

        // do the actual insert into the note
        QString buffer;
        QByteArray hash(newRes.data.bodyHash.c_str(), newRes.data.bodyHash.size());
        buffer.append("<img src=\"file://");
        buffer.append(path);
        buffer.append("\" type=\"");
        buffer.append(mime);
        buffer.append("\" hash=\"");
        buffer.append(hash.toHex());
        buffer.append("\" onContextMenu=\"window.browser.imageContextMenu(&apos;");
        buffer.append(QString::number(rlid));
        buffer.append("&apos;, &apos;");
        buffer.append(g);
        buffer.append("&apos;);\" ");
        buffer.append(" en-tag=\"en-media\" style=\"cursor: default;\" lid=\"");
        buffer.append(QString::number(rlid));
        buffer.append("\">");

        // Insert the actual image
        editor->page()->mainFrame()->evaluateJavaScript(
                script_start + buffer + script_end);
        return;
    }

    if (mime == "application/pdf") {
        // The resource is done, now we need to add it to the
        // note body
        QString g =  QString::number(rlid)+extension;

        // do the actual insert into the note
        QString buffer;
        QByteArray hash(newRes.data.bodyHash.c_str(), newRes.data.bodyHash.size());
        buffer.append("<object width=\"100%\" height=\"100%\" lid=\"" +QString::number(rlid) +"\" hash=\"");
        buffer.append(hash.toHex());
        buffer.append("\" type=\"application/pdf\" />");

        // Insert the actual image
        editor->page()->mainFrame()->evaluateJavaScript(
                script_start + buffer + script_end);
        return;

    }

    // If we have something other than an image or PDF
    // First get the icon for this type of file
    AttachmentIconBuilder builder;
    QString g =  global.fileManager.getDbaDirPath()+ QString::number(rlid)+extension;
    QString tmpFile = builder.buildIcon(rlid, filename);

    // do the actual insert into the note
    QString buffer;
    buffer.append("<a en-tag=\"en-media\" ");
    buffer.append("lid=\""+QString::number(rlid) +QString("\" "));
    buffer.append("type=\"" +mime +"\" ");
    buffer.append("hash=\"" +hash.toHex()+"\" ");
    buffer.append("href=\"nnres:" +g+"\" ");
    buffer.append("oncontextmenu=\"window.browserWindow.resourceContextMenu(&apos");
    buffer.append(g +QString("&apos);\" "));
    buffer.append(">");

    buffer.append("<img en-tag=\"temporary\" title=\""+QFileInfo(filename).fileName() +"\" ");
    buffer.append("src=\"file://");
    buffer.append(tmpFile);
    buffer.append("\" />");
    buffer.append("</a>");

    // Insert the actual attachment
    editor->page()->mainFrame()->evaluateJavaScript(
            script_start + buffer + script_end);
}


