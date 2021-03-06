
#include "communicationmanager.h"
#include "oauth/oauthtokenizer.h"
#include "global.h"

#include <execinfo.h>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include "evernote/UserStore_constants.h"
#include "sql/resourcetable.h"
#include "sql/tagtable.h"
#include <sql/usertable.h>
#include <sql/notetable.h>
#include <QObject>
#include <QPainter>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <execinfo.h>

extern Global global;


// Generic constructor
CommunicationManager::CommunicationManager(QSqlDatabase *db)
{
    initComplete = false;
    this->db = db;
    evernoteHost = global.server.toStdString();
    userStorePath = "/edam/user";
    clientName = "NixNote/Linux";
    networkAccessManager = new QNetworkAccessManager();
    inkNoteList = new QList< QPair<QString, QImage*>* >();
    thumbnailList = new QList< QPair<QString, QImage*>* >();

    sslSocketFactory = shared_ptr<TSSLSocketFactory>(new TSSLSocketFactory());

    // Load certificates
    QDir myDir(global.getProgramDirPath()+"/certs");
    QStringList filter;
    filter.append("*.pem");
    QStringList list = myDir.entryList(filter, QDir::Files, QDir::NoSort);	// filter resource files
    for (int i=0; i<list.size(); i++) {
        sslSocketFactory->loadTrustedCertificates((global.getProgramDirPath()+"/certs/"+list[i]).toStdString().c_str());
    }

//    QString pgmDir = global.getProgramDirPath() + "/certs/verisign_certs.pem";
//    sslSocketFactory->loadTrustedCertificates(pgmDir.toStdString().c_str());
//    pgmDir = global.getProgramDirPath() + "/certs/thawte.pem";
//    sslSocketFactory->loadTrustedCertificates(pgmDir.toStdString().c_str());
    sslSocketFactory->authenticate(true);
    postData = new QUrl();
}



// Destructor
CommunicationManager::~CommunicationManager() {
    if (sslSocketUserStore != NULL) {
        sslSocketUserStore->setRecvTimeout(10);
        sslSocketUserStore->close();
    }
    if (sslSocketNoteStore != NULL) {
        sslSocketNoteStore->setRecvTimeout(10);
        sslSocketNoteStore->close();
    }
    delete postData;
}


// Connect to Evernote
bool CommunicationManager::connect() {
    // Get the oAuth token
    OAuthTokenizer tokenizer;
    QString data = global.accountsManager->getOAuthToken();
    tokenizer.tokenize(data);
    authToken = tokenizer.oauth_token.toStdString();
    return init();
}


// Get the current sync state
bool CommunicationManager::getSyncState(string token, SyncState &syncState, int errorCount) {
    if (token == "")
        token = authToken;
    try {
        noteStoreClient->getSyncState(syncState, token);
        QLOG_DEBUG() << "New count: "<< syncState.updateCount;
        return true;
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        return false;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return false;
    } catch (TTransportException e) {
        QLOG_ERROR() << "TTransport error: Type->" << e.getType();
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return getSyncState(token, syncState, errorCount);
        }
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        error.message = tr("Transport error getting sync state: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::TTransportException;
        return false;
    } catch (std::exception e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return getSyncState(token, syncState, errorCount);
        }
        QLOG_ERROR() << "Standard exception:" << e.what() << endl;
        error.message = tr("Std Error getting sync state: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::StdException;
        return false;
    } catch (...) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return getSyncState(token, syncState, errorCount);
        }
        QLOG_ERROR() << "Unhandled exception:" << endl;
        error.message = tr("Unknown error getting sync state");
        error.type = CommunicationError::Unknown;
        return false;
    }
    return true;
}


// Get a sync chunk
bool CommunicationManager::getSyncChunk(SyncChunk &chunk, int start, int chunkSize, int type, bool fullSync, int errorCount) {
    // Get rid of old stuff from last chunk
    while(inkNoteList->size() > 0) {
        QPair<QString, QImage*> *pair = inkNoteList->takeLast();
        delete pair->second;
        delete pair;
    }
    inkNoteList->empty();

    bool notebooks = false;
    bool searches = false;
    bool tags = false;
    bool linkedNotebooks = false;
    bool notes = false;
    bool resources = false;
    bool expunged = false;

    notebooks = ((type & SYNC_CHUNK_NOTEBOOKS)>0);
    searches = ((type & SYNC_CHUNK_SEARCHES)>0);
    tags = ((type & SYNC_CHUNK_TAGS)>0);
    linkedNotebooks = ((type & SYNC_CHUNK_LINKED_NOTEBOOKS)>0);
    notes = ((type & SYNC_CHUNK_NOTES)>0);
    expunged = ((type & SYNC_CHUNK_NOTES) && (!fullSync)>0);
    resources = ((type & SYNC_CHUNK_RESOURCES)>0);
    if (fullSync)
        resources = false;

    // Try to get the chunk
    try {
        SyncChunkFilter filter;
        filter.__isset.includeExpunged = true;
        filter.__isset.includeNotes = true;
        filter.__isset.includeNoteResources = true;
        filter.__isset.includeNoteAttributes = true;
        filter.__isset.includeNotebooks = true;
        filter.__isset.includeTags = true;
        filter.__isset.includeSearches = true;
        filter.__isset.includeResources = true;
        filter.__isset.includeLinkedNotebooks = true;
        filter.__isset.includeNoteApplicationDataFullMap = true;
        filter.__isset.includeNoteResourceApplicationDataFullMap = true;
        filter.__isset.includeNoteResourceApplicationDataFullMap =true;

        filter.includeExpunged = notes;
        filter.includeNotes = notes;
        filter.includeNoteResources = fullSync;
        filter.includeNoteAttributes = notes;
        filter.includeNotebooks = notebooks;
        filter.includeTags = tags;
        filter.includeSearches = searches;
        filter.includeResources = resources;
        filter.includeLinkedNotebooks = linkedNotebooks;
        filter.includeNoteApplicationDataFullMap = false;
        filter.includeNoteResourceApplicationDataFullMap = false;
        filter.includeNoteResourceApplicationDataFullMap = false;

        // This is a failsafe to prevnt loops if nothing passes the filter
        chunk.chunkHighUSN = chunk.updateCount;

        // Get the actual chunk.
        noteStoreClient->getFilteredSyncChunk(chunk, authToken, start, chunkSize, filter);

        QHash<QString,QString> noteList;
        if (fullSync)
            resources = true;
        for (unsigned int i=0; chunk.__isset.notes && i<chunk.notes.size(); i++) {
            QLOG_TRACE() << "Fetching chunk item: " << i << ": " << QString::fromStdString(chunk.notes[i].title);
            Note n;
            noteList.insert(QString::fromStdString(n.guid),"");
            noteStoreClient->getNote(n, authToken, chunk.notes[i].guid, true, resources, resources, resources);
            QLOG_TRACE() << "Note Retrieved";
            TagTable tagTable(db);
            for (unsigned int j=0; j<n.tagGuids.size(); j++) {
                Tag tag;
                if (tagTable.get(tag, n.tagGuids[j]))
                    n.tagNames.push_back(tag.name);
            }
            chunk.notes[i] = n;
            if (n.__isset.resources && n.resources.size() > 0) {
                QLOG_TRACE() << "Checking for ink note";
                checkForInkNotes(n.resources, "", QString::fromStdString(authToken));
            }
            QLOG_TRACE() << "Downloading thumbnail";
//            downloadThumbnail(QString::fromStdString(n.guid), authToken,"");
        }


        QLOG_DEBUG() << "All notes retrieved.  Getting resources";
//        NoteTable noteTable(db);
        for (unsigned int i=0; chunk.__isset.resources && i<chunk.resources.size(); i++) {
            QLOG_TRACE() << "Fetching chunk resource item: " << i << ": " << QString::fromStdString(chunk.resources[i].guid);
            Resource r;
            noteStoreClient->getResource(r, authToken, chunk.resources[i].guid, true, true, true, true);
            QLOG_TRACE() << "Resource retrieved";
            chunk.resources[i] = r;

//            if (noteTable.getLid(QString::fromStdString(r.noteGuid))<=0 || !noteList.contains(QString::fromStdString(r.noteGuid))) {
//                Note n;
//                noteStoreClient->getNote(n, authToken, r.noteGuid, true, true, true, true);
//                chunk.__isset.notes = true;
//                chunk.notes.push_back(n);
//            }
        }
        QLOG_DEBUG() << "Getting ink notes";
        if (chunk.__isset.resources && chunk.resources.size()>0) {
            QLOG_TRACE() << "Checking for ink notes";
            checkForInkNotes(chunk.resources,"", QString::fromStdString(authToken));
        }
    } catch (EDAMUserException e) {
        error.message = tr("EDAMUserException ") + QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMUserException;
        error.code = -1;
        return false;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return false;
    } catch (TTransportException e) {
        QLOG_ERROR() << "TTransport error: Type->" << e.getType();
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return getSyncChunk(chunk, start, chunkSize, fullSync, errorCount);
        }
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        error.message = tr("Error getting sync chunk: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::TTransportException;
        return false;
    } catch (std::exception e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return getSyncChunk(chunk, start, chunkSize, fullSync, errorCount);
        }
        QLOG_ERROR() << "Standard exception:" << e.what() << endl;
        error.message = tr("Error getting sync chunk: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::StdException;
        return false;
    } catch (...) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return getSyncChunk(chunk, start, chunkSize, fullSync, errorCount);
        }
        QLOG_ERROR() << "Unhandled exception:" << endl;
        error.message = tr("Unknown error getting sync chunk");
        error.type = CommunicationError::Unknown;
        return false;
    }
    QLOG_DEBUG() << "Chunk complete";
    return true;
}



// Get a shared notebook by authentication token
bool CommunicationManager::getSharedNotebookByAuth(SharedNotebook &sharedNotebook) {
    try {
        linkedNoteStoreClient->getSharedNotebookByAuth(sharedNotebook, linkedAuthToken.authenticationToken);
        return true;
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        return false;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return false;
    } catch (TTransportException e) {
        QLOG_ERROR() << "TTransport error: Type->" << e.getType();
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        return false;
    }
}


// Authenticate to a linked notebook
bool CommunicationManager::authenticateToLinkedNotebookShard(LinkedNotebook book) {


    QLOG_DEBUG() << "Inside CommunicationManager::authenticateToLinkedNotebook()";

    disconnectFromLinkedNotebook();

    // This is a bit of a workaround.  If the notebook is a privately shared notebook, then
    // we can use https to connect.  If it is a public notebook then we use unencrypted
    // http.
    //
    // For public notebooks we don't need to call the authenticateToSharedNotebook function.
    //
    // If I try to use an https connection for a public notebook, the function
    // getLinkedNotebookSyncState will fail with some bizzaro SSL error.  I have no
    // idea why.  This same connection works using http.  Since this is a "public"
    // notebook we should be o.k. with an unencrypted connection.

    try {
        if (book.shareKey != "") {

            linkedSslSocketNoteStore = sslSocketFactory->createSocket(evernoteHost, 443);
            shared_ptr<TBufferedTransport> bufferedTransport(new TBufferedTransport(linkedSslSocketNoteStore));

            linkedNoteStorePath = "/edam/note/" +book.shardId;
            linkedNoteStoreHttpClient = shared_ptr<TTransport>(new THttpClient(bufferedTransport, evernoteHost, linkedNoteStorePath));

            linkedNoteStoreHttpClient->open();

            shared_ptr<TProtocol> noteStoreProtocol(new TBinaryProtocol(linkedNoteStoreHttpClient));
            linkedNoteStoreClient = shared_ptr<NoteStoreClient>(new NoteStoreClient(noteStoreProtocol));

            linkedNoteStoreClient->authenticateToSharedNotebook(linkedAuthToken, book.shareKey, authToken);
        } else {
            // This is a workaround to force an http connection for public notebooks to avoid errors
            // getting the sync state.
            string path = linkedNoteStorePath = "/edam/note/" +book.shardId;
            shared_ptr<THttpClient> client(new THttpClient(evernoteHost, 80, path));
            client->open();

            shared_ptr<TProtocol> noteStoreProtocol(new TBinaryProtocol(client));
            linkedNoteStoreClient = shared_ptr<NoteStoreClient>(new NoteStoreClient(noteStoreProtocol));
        }

    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.what() << endl;
        return false;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return false;
    } catch (TTransportException e) {
        QLOG_ERROR() << "TTransport error: Type->" << e.getType();
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        return false;
    }
    QLOG_DEBUG() << "Leaving CommunicationManager::authenticateToLinkedNotebookShard()";
    return true;
}



// Get a linked notebook's sync state
bool CommunicationManager::getLinkedNotebookSyncState(SyncState &syncState, LinkedNotebook linkedNotebook, int errorCount) {
    try {
        linkedNoteStoreClient->getLinkedNotebookSyncState(syncState, linkedAuthToken.authenticationToken, linkedNotebook);
        QLOG_DEBUG() << "New linked notebook count: "<< syncState.updateCount;
        return true;
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        return false;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return false;
    } catch (TTransportException e) {
       QLOG_ERROR() << "TTransport error: Type->" << e.getType();
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           authenticateToLinkedNotebookShard(linkedNotebook);
           return getLinkedNotebookSyncState(syncState, linkedNotebook, errorCount);
       }
       QLOG_ERROR() << "TTransportException:" << e.what() << endl;
       error.message = tr("Transport error getting linked notebook state: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::TTransportException;
       return 0;
   } catch (std::exception e) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           authenticateToLinkedNotebookShard(linkedNotebook);
           return getLinkedNotebookSyncState(syncState, linkedNotebook, errorCount);
       }
       QLOG_ERROR() << "Standard exception:" << e.what() << endl;
       error.message = tr("Error getting linked notebook state: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::StdException;
       return 0;
   } catch (...) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return getLinkedNotebookSyncState(syncState, linkedNotebook, errorCount);
       }
       QLOG_ERROR() << "Unhandled exception:" << endl;
       error.message = tr("Unknown error getting linked notebook state");
       error.type = CommunicationError::Unknown;
       return 0;
   }
}



// Get a linked notebook's sync chunk
bool CommunicationManager::getLinkedNotebookSyncChunk(SyncChunk &chunk, LinkedNotebook linkedNotebook, int start, int chunkSize, bool fullSync, int errorCount) {

    // Get rid of old stuff from last chunk
    while(inkNoteList->size() > 0) {
        QPair<QString, QImage*> *pair = inkNoteList->takeLast();
        delete pair->second;
        delete pair;
    }
    inkNoteList->empty();

    // Try to get the chunk
    QHash<QString,QString> noteList;
    try {
        linkedNoteStoreClient->getLinkedNotebookSyncChunk(chunk, authToken, linkedNotebook, start, chunkSize, fullSync);
        for (unsigned int i=0; chunk.__isset.notes && i<chunk.notes.size(); i++) {
            QLOG_TRACE() << "Fetching chunk item: " << i << ": " << QString::fromStdString(chunk.notes[i].title);
            Note n;
            linkedNoteStoreClient->getNote(n, linkedAuthToken.authenticationToken, chunk.notes[i].guid, true, fullSync, fullSync, fullSync);
           // n.notebookGuid = linkedNotebook.guid;
            noteList.insert(QString::fromStdString(n.guid),"");
            TagTable tagTable(db);
            for (unsigned int j=0; j<n.tagGuids.size(); j++) {
                Tag tag;
                if (tagTable.get(tag, n.tagGuids[j]))
                    n.tagNames.push_back(tag.name);
            }
            chunk.notes[i] = n;
            if (n.__isset.resources && n.resources.size() > 0) {
                checkForInkNotes(n.resources, QString::fromStdString(linkedNotebook.shardId), QString::fromStdString(authToken));
            }
            downloadThumbnail(QString::fromStdString(n.guid), authToken, linkedNotebook.shardId);
        }

        // Fetch resources
        for (unsigned int i=0; chunk.__isset.resources && i<chunk.resources.size(); i++) {
            QLOG_TRACE() << "Fetching chunk resource item: " << i << ": " << QString::fromStdString(chunk.resources[i].guid);
            Resource r;
            linkedNoteStoreClient->getResource(r, linkedAuthToken.authenticationToken, chunk.resources[i].guid, true, true, true, true);
            chunk.resources[i] = r;

            NoteTable noteTable(db);
            for (unsigned int i=0; chunk.__isset.resources && i<chunk.resources.size(); i++) {
                QLOG_TRACE() << "Fetching chunk resource item: " << i << ": " << QString::fromStdString(chunk.resources[i].guid);
                Resource r;
                noteStoreClient->getResource(r, authToken, chunk.resources[i].guid, true, true, true, true);
                QLOG_TRACE() << "Resource retrieved";
                chunk.resources[i] = r;

                if (noteTable.getLid(QString::fromStdString(r.noteGuid))<=0 || !noteList.contains(QString::fromStdString(r.noteGuid))) {
                    Note n;
                    noteStoreClient->getNote(n, authToken, r.noteGuid, true, true, true, true);
                    chunk.__isset.notes = true;
                    chunk.notes.push_back(n);
                }
            }
        }
        if (chunk.__isset.resources && chunk.resources.size()>0)
            checkForInkNotes(chunk.resources, QString::fromStdString(linkedNotebook.shardId), QString::fromStdString(linkedAuthToken.authenticationToken));
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.message = tr("Shared notebook EDAMUserException ") + QString::number(e.errorCode);
        error.type = CommunicationError::EDAMUserException;
        error.code = e.errorCode;
        return false;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return false;
    } catch (TTransportException e) {
        QLOG_ERROR() << "TTransport error: Type->" << e.getType();
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying linked notebook";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            authenticateToLinkedNotebookShard(linkedNotebook);
            return getLinkedNotebookSyncChunk(chunk, linkedNotebook, start,  chunkSize, fullSync, errorCount);
        }
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        return false;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.identifier) << ":" << QString::fromStdString(e.key) << endl;
        error.message = tr("Shared notebook ") + QString::fromStdString(e.key) + tr(" not found.");
        error.type = CommunicationError::EDAMNotFoundException;
        error.code = -1;
        return false;
    } catch (TException e) {
        QLOG_ERROR() << "TException:" << e.what();
        error.message = tr("Shared notebook TException ") + QString::fromStdString(e.what());
        error.type = CommunicationError::TException;
        error.code = -1;
        return false;
    } catch (std::exception e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying linked notebook";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            authenticateToLinkedNotebookShard(linkedNotebook);
            return getLinkedNotebookSyncChunk(chunk, linkedNotebook, start,  chunkSize, fullSync, errorCount);
        }
        QLOG_ERROR() << "Standard exception:" << e.what() << endl;
        return false;
    } catch (...) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying linked notebook";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            authenticateToLinkedNotebookShard(linkedNotebook);
            return getLinkedNotebookSyncChunk(chunk, linkedNotebook, start,  chunkSize, fullSync, errorCount);
        }
        QLOG_ERROR() << "Unhandled exception:" << endl;
        return false;
    }
    return true;
}




// start the communication session
bool CommunicationManager::init() {
    if (initComplete)
        return true;
    if (!initNoteStore())
        return false;
    initComplete = true;
    return true;
}




// helper function to get an auth token
string CommunicationManager::getToken() {
    //return authenticationResult->authenticationToken;
    return authToken;
}


// Initialize the user store (contains user account information)
bool CommunicationManager::initUserStore() {
    QLOG_DEBUG() << "Inside CommunicationManager::initUserStore()";
    try {
        sslSocketUserStore = sslSocketFactory->createSocket(evernoteHost, 443);
        //sslSocketUserStore->setNoDelay(true);
        SOCKET s = sslSocketUserStore->getSocketFD();
        this->setSocketOptions(s);

        shared_ptr<TBufferedTransport> bufferedTransport(new TBufferedTransport(sslSocketUserStore));
        userStoreHttpClient = shared_ptr<TTransport>(new THttpClient(bufferedTransport, evernoteHost, userStorePath));

        userStoreHttpClient->open();
        shared_ptr<TProtocol> iprot(new TBinaryProtocol(userStoreHttpClient));
        userStoreClient = shared_ptr<UserStoreClient>(new UserStoreClient(iprot));
        UserStoreConstants version;
        if (!userStoreClient->checkVersion(clientName, version.EDAM_VERSION_MAJOR, version.EDAM_VERSION_MINOR)) {
                QLOG_ERROR() << "Incompatible Evernote API version";
                return false;
        }

    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMUserException;
        return false;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return false;
    } catch (TTransportException e) {
        QLOG_ERROR() << "TTransport error: Type->" << e.getType();
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::TTransportException;
        return false;
    }
    QLOG_DEBUG() << "Leaving CommunicationManager::initUserStore()";
    return true;
}



void CommunicationManager::setSocketOptions(SOCKET s) {
    int optval;

    socklen_t optlen = sizeof(optval);
    getsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &optval, &optlen);
    optval = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &optval, optlen);

    getsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &optval, &optlen);
    optval=9;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &optval, optlen);

    getsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &optval, &optlen);
    optval=1200;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &optval, optlen);

    getsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &optval, &optlen);
    optval=60;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &optval, optlen);
}

// Initialize the note store
bool CommunicationManager::initNoteStore() {
    QLOG_DEBUG() << "Inside CommunicationManager::initNoteStore()";

    try {
        shared_ptr<TSSLSocketFactory> sslSocketFactory(new TSSLSocketFactory());

        // Load certificates
        QDir myDir(global.getProgramDirPath()+"/certs");
        QStringList filter;
        filter.append("*.pem");
        QStringList list = myDir.entryList(filter, QDir::Files, QDir::NoSort);	// filter resource files
        for (int i=0; i<list.size(); i++) {
            sslSocketFactory->loadTrustedCertificates((global.getProgramDirPath()+"/certs/"+list[i]).toStdString().c_str());
        }

//        QString pgmDir = global.getProgramDirPath() + "/certs/thawte.pem";
//        sslSocketFactory->loadTrustedCertificates(pgmDir.toStdString().c_str());
//       pgmDir = global.getProgramDirPath() + "/certs/verisign_certs.pem";
//        sslSocketFactory->loadTrustedCertificates(pgmDir.toStdString().c_str());
        sslSocketFactory->authenticate(true);

        sslSocketNoteStore = sslSocketFactory->createSocket(evernoteHost, 443);
        //sslSocketNoteStore->setNoDelay(true);
        SOCKET s = sslSocketNoteStore->getSocketFD();
        this->setSocketOptions(s);

        shared_ptr<TBufferedTransport> bufferedTransport(new TBufferedTransport(sslSocketNoteStore));
        User user;
        if (!getUserInfo(user))
            return false;
        noteStorePath = "/edam/note/" +user.shardId;
        noteStoreHttpClient = shared_ptr<TTransport>(new THttpClient(bufferedTransport, evernoteHost, noteStorePath));

        noteStoreHttpClient->open();
        shared_ptr<TProtocol> noteStoreProtocol(new TBinaryProtocol(noteStoreHttpClient));
        noteStoreClient = shared_ptr<NoteStoreClient>(new NoteStoreClient(noteStoreProtocol));

        SyncState syncState;
        noteStoreClient->getSyncState(syncState, authToken);

    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMUserException;
        return false;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return false;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.what()) << endl;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMNotFoundException;
        return false;
    } catch (TTransportException e) {
        QLOG_ERROR() << "TTransport error: Type->" << e.getType();
        QLOG_ERROR() << "\n\nTTransportException:" << e.what() << endl;
        error.message = QString(e.what());
        error.message = QString::fromStdString(e.what());
        return false;
    }
    QLOG_DEBUG() << "Leaving CommunicationManager::initNoteStore()";
    return true;
}



// Disconnect from Evernote's servers (for private notebooks)
void CommunicationManager::disconnect() {

    QLOG_DEBUG() << "Disconnecting";
    try {
        if (noteStoreHttpClient != NULL && noteStoreHttpClient->isOpen()) {
                noteStoreHttpClient->flush();
            noteStoreHttpClient->close();
        }
    } catch (std::exception e) {
        QLOG_DEBUG() << "Std exception disconnecting from notestore. " << e.what();
    } catch (...) {
        QLOG_DEBUG() << "Unknown exception disconnecting from notestore. ";
    }
    disconnectFromLinkedNotebook();
    initComplete=false;
}

// Disconnect from the user store
void CommunicationManager::disconnectUserStore() {
    try {
        if (userStoreHttpClient != NULL && userStoreHttpClient->isOpen()) {
                userStoreHttpClient->flush();
            userStoreHttpClient->close();
        }
    } catch (std::exception e) {
        QLOG_DEBUG() << "Std exception disconnecting from userstore. " << e.what();
    } catch (...) {
        QLOG_DEBUG() << "Unknown exception disconnecting from userstore. ";
    }
}



// Disconnect from Evernote's servers (for linked notebooks)
void  CommunicationManager::disconnectFromLinkedNotebook() {
    QLOG_DEBUG() << "Disconnecting from linked notebook";
    try {
        if (linkedNoteStoreHttpClient != NULL && linkedNoteStoreHttpClient->isOpen()) {
            linkedNoteStoreHttpClient->flush();
            linkedNoteStoreHttpClient->close();
        }
    } catch (std::exception e) {
        QLOG_DEBUG() << "Std exception disconnecting from linked notebook. " << e.what();
    } catch (...) {
        QLOG_DEBUG() << "Unknown exception disconnecting from linked notebook. ";
    }
}




// Get a user's information
bool CommunicationManager::getUserInfo(User &user, int errorCount) {
    QLOG_DEBUG() << "Inside CommunicationManager::getUserInfo";
    try {
       //this->refreshConnection();
        if (!initUserStore())
            return false;
       userStoreClient->getUser(user, authToken);
       disconnectUserStore();
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMUserException;
        disconnectUserStore();
        return false;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        disconnectUserStore();
        return false;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.what()) << endl;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMNotFoundException;
        disconnectUserStore();
        return false;
    } catch (TTransportException e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return getUserInfo(user, errorCount);
        }
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        error.message = tr("Error getting sync chunk: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::TTransportException;
        return false;
    } catch (std::exception e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return getUserInfo(user, errorCount);
        }
        QLOG_ERROR() << "Standard exception:" << e.what() << endl;
        error.message = tr("Error getting sync chunk: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::StdException;
        return false;
    } catch (...) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return getUserInfo(user, errorCount);
        }
        QLOG_ERROR() << "Unhandled exception:" << endl;
        error.message = tr("Unknown error getting sync chunk");
        error.type = CommunicationError::Unknown;
        return false;
    }
    return true;
}




// See if there are any ink notes in this list of resources
void CommunicationManager::checkForInkNotes(vector<Resource> &resources, QString shard, QString authToken) {
    for (unsigned int i=0; i<resources.size(); i++) {
        Resource *r = &resources[i];
        if (r->__isset.mime && r->mime == "application/vnd.evernote.ink") {
            downloadInkNoteImage(QString::fromStdString(r->guid), r, shard, authToken);
        }
    }
}



// Download an ink note image
void CommunicationManager::downloadInkNoteImage(QString guid, Resource *r, QString shard, QString authToken) {
    UserTable userTable(db);
    User u;
    userTable.getUser(u);
    if (shard == "")
        shard = QString::fromStdString(u.shardId);
    QString urlBase = QString::fromStdString("https://")+QString::fromStdString(evernoteHost)
            +QString("/shard/")
            +shard
            +QString("/res/")
            +guid +QString(".ink?slice=");
    int sliceCount = 1+((r->height-1)/600);
    QSize size;
    size.setHeight(r->height);
    size.setWidth(r->width);
    postData->clear();
    postData->addQueryItem("auth", authToken);

    QEventLoop loop;
    QObject::connect(networkAccessManager, SIGNAL(finished(QNetworkReply*)), &loop, SLOT(quit()));

    int position = 0;
    QImage *newImage = NULL;
    for (int i=0; i<sliceCount && position >=0; i++) {
        QUrl url(urlBase+QString::number(i+1));

        QNetworkRequest request(url);

        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
        QNetworkReply *reply = networkAccessManager->post(request,postData->encodedQuery());

        // Execute the event loop here, now we will wait here until readyRead() signal is emitted
        // which in turn will trigger event loop quit.
        loop.exec();
        QImage replyImage;
        replyImage.loadFromData(reply->readAll());
        if (newImage == NULL)
            newImage= new QImage(size, replyImage.format());
        position = inkNoteReady(newImage, &replyImage, position);
        if (position == -1) {
            QLOG_ERROR() << "Error fetching ink note slice " << reply->errorString();
        }
    }
    QPair<QString, QImage*> *newPair = new QPair<QString, QImage*>();
    newPair->first = guid;
    newPair->second = newImage;
    inkNoteList->append(newPair);

    QObject::disconnect(&loop, SLOT(quit()));
}



// An ink note image is ready for retrieval
int CommunicationManager::inkNoteReady(QImage *img, QImage *replyImage, int position) {
    int priorPosition = position;
    position = position+replyImage->height();
    if (!replyImage->isNull()) {
        QPainter p(img);
        p.drawImage(QRect(0,priorPosition, replyImage->width(), position), *replyImage);
        p.end();
        return position;
    }
    return -1;
}




// Upload a new/changed saved search
qint32 CommunicationManager::uploadSavedSearch(SavedSearch &search, int errorCount) {
    // Try upload
    try {
        if (search.updateSequenceNum > 0)
            return noteStoreClient->updateSearch(authToken, search);
        else {
            noteStoreClient->createSearch(search, authToken, search);
            return search.updateSequenceNum;
        }
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = tr("Error uploading saved search ") + QString::fromStdString(search.name) + " : " +QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMUserException;
        return 0;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return 0;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.what()) << endl;
        error.message = tr("Error uploading saved search ") + QString::fromStdString(search.name) + " : " +QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMNotFoundException;
        return 0;
    } catch (TTransportException e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return uploadSavedSearch(search, errorCount);
        }
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        error.message = tr("Transport error uploading saved search \"") +QString::fromStdString(search.name) +"\" " +QString::fromStdString(e.what());
        error.type = CommunicationError::TTransportException;
        return 0;
    } catch (std::exception e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return uploadSavedSearch(search, errorCount);
        }
        QLOG_ERROR() << "Standard exception:" << e.what() << endl;
        error.message = tr("Std error uploading saved search \"") +QString::fromStdString(search.name) +"\" " +QString::fromStdString(e.what());
        error.type = CommunicationError::StdException;
        return 0;
    } catch (...) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return uploadSavedSearch(search, errorCount);
        }
        QLOG_ERROR() << "Unhandled exception:" << endl;
        error.message = tr("Unknown error uploading saved search \"") +QString::fromStdString(search.name) +"\"";
        error.type = CommunicationError::Unknown;
        return 0;
    }
    return true;}



// Permanently delete a saved search
qint32 CommunicationManager::expungeSavedSearch(string guid, int errorCount) {
    // Try upload
    try {
        return noteStoreClient->expungeSearch(authToken, guid);
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMUserException;
        return 0;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return 0;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.what()) << endl;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMNotFoundException;
        return 0;
    } catch (TTransportException e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return expungeSavedSearch(guid, errorCount);
        }
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        error.message = tr("Transport error deleting saved search: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::TTransportException;
        return false;
    } catch (std::exception e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Standard error deleting saved search #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return expungeSavedSearch(guid, errorCount);
        }
        QLOG_ERROR() << "Standard exception:" << e.what() << endl;
        error.message = tr("Error deleting saved search: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::StdException;
        return false;
    } catch (...) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Unhandled exception deleting saved search #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return expungeSavedSearch(guid, errorCount);
        }
        QLOG_ERROR() << "Unhandled exception:" << endl;
        error.message = tr("Unknown error getting expunging saved search");
        error.type = CommunicationError::Unknown;
        return false;
    }
    return true;
}



// Upload a new/changed tag to Evernote
qint32 CommunicationManager::uploadTag(Tag &tag, int errorCount) {
    // Try upload
    try {
        if (tag.updateSequenceNum > 0)
            return noteStoreClient->updateTag(authToken, tag);
        else {
            noteStoreClient->createTag(tag, authToken, tag);
            return tag.updateSequenceNum;
        }
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException: " << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = tr("Error uploading tag ") + QString::fromStdString(tag.name) + " : " +QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMUserException;
        return 0;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return 0;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.what()) << endl;
        error.message = tr("Error uploading tag ") + QString::fromStdString(tag.name) + " : "+ QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMNotFoundException;
        return 0;
    } catch (TTransportException e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return uploadTag(tag, errorCount);
        }
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        error.message = tr("Transport error uploading tag: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::TTransportException;
        return 0;
    } catch (std::exception e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return uploadTag(tag, errorCount);
        }
        QLOG_ERROR() << "Standard exception:" << e.what() << endl;
        error.message = tr("Error uploading tag: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::StdException;
        return 0;
    } catch (...) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return uploadTag(tag, errorCount);
        }
        QLOG_ERROR() << "Unhandled exception:" << endl;
        error.message = tr("Unknown error uploading tag");
        error.type = CommunicationError::Unknown;
        return 0;
    }
}


// Permanently delete a tag from Evernote
qint32 CommunicationManager::expungeTag(string guid, int errorCount) {
    // Try upload
    try {
        return noteStoreClient->expungeTag(authToken, guid);
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMUserException;
        return 0;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return 0;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.what()) << endl;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMNotFoundException;
        return 0;
    } catch (TTransportException e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return expungeTag(guid, errorCount);
        }
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        error.message = tr("Error expunging tag: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::TTransportException;
        return 0;
    } catch (std::exception e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return expungeTag(guid, errorCount);
        }
        QLOG_ERROR() << "Standard exception:" << e.what() << endl;
        error.message = tr("Error expunging tag: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::StdException;
        return 0;
    } catch (...) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return expungeTag(guid, errorCount);
        }
        QLOG_ERROR() << "Unhandled exception:" << endl;
        error.message = tr("Unknown error expunging tag");
        error.type = CommunicationError::Unknown;
        return 0;
    }
}



// Upload a notebook to Evernote
qint32 CommunicationManager::uploadNotebook(Notebook &notebook, int errorCount) {
    // Try upload
    try {
        if (notebook.updateSequenceNum > 0)
            return noteStoreClient->updateNotebook(authToken, notebook);
        else {
            noteStoreClient->createNotebook(notebook, authToken, notebook);
            return notebook.updateSequenceNum;
        }
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = tr("Error uploading notebook ") + QString::fromStdString(notebook.name) + " : " +QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMUserException;
        return 0;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return 0;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.what()) << endl;
        error.message = tr("Error uploading notebook ") + QString::fromStdString(notebook.name) + " : " +QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMNotFoundException;
        return 0;
     } catch (TTransportException e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return uploadNotebook(notebook, errorCount);
        }
        QLOG_ERROR() << "TTransportException:" << e.what() << endl;
        error.message = tr("Transport error uploading notebook: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::TTransportException;
        return 0;
    } catch (std::exception e) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return uploadNotebook(notebook, errorCount);
        }
        QLOG_ERROR() << "Standard exception:" << e.what() << endl;
        error.message = tr("Error uploading notebook: ") +QString::fromStdString(e.what());
        error.type = CommunicationError::StdException;
        return 0;
    } catch (...) {
        if (errorCount < 3) {
            errorCount++;
            QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
            disconnect();
            QLOG_ERROR() << "Reconnecting";
            connect();
            return uploadNotebook(notebook, errorCount);
        }
        QLOG_ERROR() << "Unhandled exception:" << endl;
        error.message = tr("Unknown error uploading notebook");
        error.type = CommunicationError::Unknown;
        return 0;
    }
}


// Permanently delete a notebook from Evernote
qint32 CommunicationManager::expungeNotebook(string guid, int errorCount) {
    // Try upload
    try {
        return noteStoreClient->expungeNotebook(authToken, guid);
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMUserException;
        return 0;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return 0;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.what()) << endl;
        error.message = QString::fromStdString(e.what());
        error.type = CommunicationError::EDAMNotFoundException;
        return 0;
    } catch (TTransportException e) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return expungeNotebook(guid, errorCount);
       }
       QLOG_ERROR() << "TTransportException:" << e.what() << endl;
       error.message = tr("Transport error expunging notebook: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::TTransportException;
       return 0;
   } catch (std::exception e) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return expungeNotebook(guid, errorCount);
       }
       QLOG_ERROR() << "Standard exception:" << e.what() << endl;
       error.message = tr("Error expunging notebook: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::StdException;
       return 0;
   } catch (...) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return expungeNotebook(guid, errorCount);
       }
       QLOG_ERROR() << "Unhandled exception:" << endl;
       error.message = tr("Unknown error expunging notebook");
       error.type = CommunicationError::Unknown;
       return 0;
   }
}



// Upload a note to Evernote
qint32 CommunicationManager::uploadNote(Note &note, int errorCount) {
    // Try upload
    try {
        if (note.updateSequenceNum > 0) {
            noteStoreClient->updateNote(note, authToken, note);
            return note.updateSequenceNum;
        } else {
            noteStoreClient->createNote(note, authToken, note);
            return note.updateSequenceNum;
        }
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = tr("Error uploading note \"") + QString::fromStdString(note.title) + "\"";
        error.type = CommunicationError::EDAMUserException;
        return 0;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return 0;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.what()) << endl;
        error.message = tr("Error uploading note \"") + QString::fromStdString(note.title) + tr("\". Note not found");
        error.type = CommunicationError::EDAMNotFoundException;
        return 0;
    } catch (TTransportException e) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return uploadNote(note, errorCount);
       }
       QLOG_ERROR() << "TTransportException:" << e.what() << endl;
       error.message = tr("Transport error uploading note: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::TTransportException;
       return 0;
   } catch (std::exception e) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return uploadNote(note, errorCount);
       }
       QLOG_ERROR() << "Standard exception:" << e.what() << endl;
       error.message = tr("Error uploading note: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::StdException;
       return 0;
   } catch (...) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return uploadNote(note, errorCount);
       }
       QLOG_ERROR() << "Unhandled exception:" << endl;
       error.message = tr("Unknown error uploading note");
       error.type = CommunicationError::Unknown;
       return 0;
   }
}



// Upload a note to Evernote
qint32 CommunicationManager::uploadLinkedNote(Note &note, LinkedNotebook linkedNotebook, int errorCount) {
    // Try upload
    try {
        if (note.updateSequenceNum > 0) {
            linkedNoteStoreClient->updateNote(note, linkedAuthToken.authenticationToken, note);
            return note.updateSequenceNum;
        } else {
            linkedNoteStoreClient->createNote(note, linkedAuthToken.authenticationToken, note);
            return note.updateSequenceNum;
        }
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = tr("Error uploading note \"") + QString::fromStdString(note.title) + "\"";
        error.type = CommunicationError::EDAMUserException;
        return 0;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return 0;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.what()) << endl;
        error.message = tr("Error uploading note \"") + QString::fromStdString(note.title) + tr("\". Note not found");
        error.type = CommunicationError::EDAMNotFoundException;
        return 0;
    } catch (TTransportException e) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           authenticateToLinkedNotebookShard(linkedNotebook);
           return uploadLinkedNote(note, linkedNotebook, errorCount);
       }
       QLOG_ERROR() << "TTransportException:" << e.what() << endl;
       error.message = tr("Transport error uploading linked note: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::TTransportException;
       return 0;
   } catch (std::exception e) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           authenticateToLinkedNotebookShard(linkedNotebook);
           return uploadLinkedNote(note, linkedNotebook, errorCount);
       }
       QLOG_ERROR() << "Standard exception:" << e.what() << endl;
       error.message = tr("Error uploading linked note: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::StdException;
       return 0;
   } catch (...) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return uploadLinkedNote(note, linkedNotebook, errorCount);
       }
       QLOG_ERROR() << "Unhandled exception:" << endl;
       error.message = tr("Unknown uploading linked note");
       error.type = CommunicationError::Unknown;
       return 0;
   }
}


// delete a note in Evernote
qint32 CommunicationManager::deleteNote(string note, int errorCount) {
    // Try upload
    try {
        qint32 usn = noteStoreClient->deleteNote(authToken, note);
        return usn;
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        error.code = e.errorCode;
        error.message = tr("Error deleting note \"") + QString::fromStdString(note) + "\"";
        error.type = CommunicationError::EDAMUserException;
        return 0;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return 0;
    } catch (EDAMNotFoundException e) {
        QLOG_ERROR() << "EDAMNotFoundException:" << QString::fromStdString(e.what()) << endl;
        error.message = tr("Error deleting note \"") + QString::fromStdString(note) + "\". Note not found";
        error.type = CommunicationError::EDAMNotFoundException;
        return 0;
    } catch (TTransportException e) {
       QLOG_ERROR() << "TTransport error: Type->" << e.getType();
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return deleteNote(note, errorCount);
       }
       QLOG_ERROR() << "TTransportException:" << e.what() << endl;
       error.message = tr("Transport error deleting note: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::TTransportException;
       return 0;
   } catch (std::exception e) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return deleteNote(note, errorCount);
       }
       QLOG_ERROR() << "Standard exception:" << e.what() << endl;
       error.message = tr("Error deleting note: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::StdException;
       return 0;
   } catch (...) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return deleteNote(note, errorCount);
       }
       QLOG_ERROR() << "Unhandled exception:" << endl;
       error.message = tr("Unknown error deleting note");
       error.type = CommunicationError::Unknown;
       return 0;
   }
}





// Download a thumbnail of the note from Evernote's servers
void CommunicationManager::downloadThumbnail(QString guid, string authToken, string shard) {
    UserTable userTable(db);
    if (shard == "") {
        User u;
        userTable.getUser(u);
        shard = u.shardId;
    }
    QString urlBase = QString::fromStdString("https://")+QString::fromStdString(evernoteHost)
            +QString("/shard/")
            +QString::fromStdString(shard)
            +QString("/thm/note/")
            +guid;
    postData->clear();
    postData->addQueryItem("auth", QString::fromStdString(authToken));

    QSize size(300,300);
    QEventLoop loop;
    QObject::connect(networkAccessManager, SIGNAL(finished(QNetworkReply*)), &loop, SLOT(quit()));

    int position = 0;
    QImage *newImage = NULL;
    QUrl url(urlBase);

    QNetworkRequest request(url);

    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    QNetworkReply *reply = networkAccessManager->post(request,postData->encodedQuery());

    // Execute the event loop here, now we will wait here until readyRead() signal is emitted
    // which in turn will trigger event loop quit.
    loop.exec();
    QImage replyImage(size, QImage::Format_ARGB32);
    replyImage.loadFromData(reply->readAll());
    newImage= new QImage(size, QImage::Format_ARGB32);
    newImage->fill(Qt::transparent);
    position = thumbnailReady(newImage, &replyImage, position);
    if (position == -1) {
        QLOG_ERROR() << "Error fetching thumbnail " << reply->errorString();
    }

    QPair<QString, QImage*> *newPair = new QPair<QString, QImage*>();
    newPair->first = guid;
    newPair->second = newImage;
    thumbnailList->append(newPair);

    QObject::disconnect(&loop, SLOT(quit()));
}



// A thumbnail is ready for retrieval from Evernote
int CommunicationManager::thumbnailReady(QImage *img, QImage *replyImage, int position) {
    int priorPosition = position;
    position = position+replyImage->height();
    if (!replyImage->isNull()) {
        QPainter p(img);
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.fillRect(0,0,img->width(),img->height(), Qt::transparent);
        img->fill(Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.drawImage(QRect(0,priorPosition, replyImage->width(), position), *replyImage);
        p.end();
        return position;
    }
    return -1;
}




// get a list of all notebooks
bool CommunicationManager::getNotebookList(vector<Notebook> &list, int errorCount) {

    // Try to get the chunk
    try {
        noteStoreClient->listNotebooks(list, authToken);
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        return false;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return false;
    } catch (TTransportException e) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return getNotebookList(list, errorCount);
       }
       QLOG_ERROR() << "TTransportException:" << e.what() << endl;
       error.message = tr("Transport error getting notebook list: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::TTransportException;
       return false;
   } catch (std::exception e) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return getNotebookList(list, errorCount);
       }
       QLOG_ERROR() << "Standard exception:" << e.what() << endl;
       error.message = tr("Error getting notebook list: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::StdException;
       return false;
   } catch (...) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return getNotebookList(list, errorCount);
       }
       QLOG_ERROR() << "Unhandled exception:" << endl;
       error.message = tr("Unknown error getting notebook list");
       error.type = CommunicationError::Unknown;
       return false;
   }
   return true;
}





// get a list of all notebooks
bool CommunicationManager::getTagList(vector<Tag> &list, int errorCount) {

    // Try to get the chunk
    try {
        noteStoreClient->listTags(list, authToken);
    } catch (EDAMUserException e) {
        QLOG_ERROR() << "EDAMUserException:" << e.errorCode << endl;
        return false;
    } catch (EDAMSystemException e) {
        QLOG_ERROR() << "EDAMSystemException";
        handleEDAMSystemException(e);
        return false;
    } catch (TTransportException e) {
       QLOG_ERROR() << "TTransport error: Type->" << e.getType();
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "TTransport error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return getTagList(list, errorCount);
       }
       QLOG_ERROR() << "TTransportException:" << e.what() << endl;
       error.message = tr("Transport error getting tag list: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::TTransportException;
       return false;
   } catch (std::exception e) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Standard exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return getTagList(list, errorCount);
       }
       QLOG_ERROR() << "Standard exception:" << e.what() << endl;
       error.message = tr("Error getting tag list: ") +QString::fromStdString(e.what());
       error.type = CommunicationError::StdException;
       return false;
   } catch (...) {
       if (errorCount < 3) {
           errorCount++;
           QLOG_ERROR() << "Unhandled exception error #" << errorCount << ".  Retrying";
           disconnect();
           QLOG_ERROR() << "Reconnecting";
           connect();
           return getTagList(list, errorCount);
       }
       QLOG_ERROR() << "Unhandled exception:" << endl;
       error.message = tr("Unknown error getting tag list");
       error.type = CommunicationError::Unknown;
       return false;
   }
   return true;
}


void CommunicationManager::handleEDAMSystemException(EDAMSystemException e) {
    void *array[30];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 30);

    // print out all the frames to stderr
    fprintf(stderr, "EDAM System Exception backtrace");
    backtrace_symbols_fd(array, size, 2);

    QLOG_ERROR() << "EDAMSystemException:" << QString::fromStdString(e.message) << endl;
    QLOG_ERROR() << "EDAMSystemException Error Code:" << e.errorCode << endl;
    QLOG_ERROR() << "EDAMSystemException Rate Limit:" << e.rateLimitDuration << endl;
    if (e.errorCode == EDAMErrorCode::RATE_LIMIT_REACHED) {
        int duration = e.rateLimitDuration/60+1;
        error.type = CommunicationError::RateLimitExceeded;
        if (duration > 1)
            error.message = tr("API rate limit exceeded.  Please try again in ") +QString::number(duration)+ tr(" minutes.");
        else
            error.message = tr("API rate limit exceeded.  Please try again in ") +QString::number(duration)+ tr(" minute.");
        return;
    }
    error.message = tr("EDAMSystemException ") + QString::fromStdString(e.message);
    error.type = CommunicationError::EDAMSystemException;
    error.code = e.errorCode;
}
