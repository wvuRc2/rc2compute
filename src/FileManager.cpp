#include "FileManager.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <event2/bufferevent.h>
#include <postgresql/libpq-fe.h>
#define BOOST_NO_CXX11_SCOPED_ENUMS
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/regex.hpp>
#include <sys/types.h>
#include <utime.h>
#include "../common/PostgresUtils.hpp"
#include "../common/FormattedException.hpp"
#include "common/RC2Utils.hpp"
#include "DBFileSource.hpp"
#include "RC2Logging.h"

using namespace std;
using boost::format;
namespace fs = boost::filesystem;

struct BoolChanger {
	bool *bptr, oldVal;
	BoolChanger(bool *var) : bptr(var) { oldVal = *bptr; *bptr = !oldVal; }
	~BoolChanger() { *bptr = oldVal; }
};


struct FSDirectory {
	int wd;
	struct stat sb;
};

struct PendingImage {
	string fileName;
	string imgNumStr;
	int wd;
	PendingImage(string fname, string iname, int desc)
		:fileName(fname), imgNumStr(iname), wd(desc)
		{}
};

using PendingImageMap = map<int,PendingImage>;

class RC2::FileManager::Impl {
	public:
		long						wspaceId_;
		long						sessionRecId_;
		long						sessionImageBatch_;
		DBFileSource				dbFileSource_;
		PGconn*						dbcon_;
		set<string>					manuallyAddedFiles_;
		map<int, DBFileInfoPtr>		filesByWatchDesc_;
		PendingImageMap				pendingImagesByWatchDesc_;
		vector<long>				imageIds_;
		string						workingDir;
		struct event_base*			eventBase_;
		struct bufferevent*			inotifyEvent_;
		int							inotifyFd_;
		FSDirectory					rootDir_;
		boost::regex				imgRegex_;
		bool						ignoreFSNotifications_;
		bool						ignoreDBNotifications_;

		Impl()
			: imgRegex_("rc2img(\\d+).png")
		{}

		void cleanup(); //replacement for destructor
		void connect(string str, long wspaceId, long sessionRecId);
		long insertImage(string fname, string imgNumStr);
		bool executeDBCommand(string cmd);
		
		unique_ptr<char[]> readFileBlob(DBFileInfoPtr fobj, size_t &size);

		void	setupInotify(FileManager *fm);
		void 	watchFile(DBFileInfoPtr file);
		void	startImageWatch(string fname, string imgNum, inotify_event *event);
		void	stopImageWatch(int wd);
		void	cleanupImageWatch();
		void	handleInotifyEvent(struct bufferevent *bev);
		static void handleInotifyEvent(struct bufferevent *bev, void *ctx)
		{
			FileManager *watcher = reinterpret_cast<FileManager*>(ctx);
			watcher->_impl->handleInotifyEvent(bev);
		}

		void processDBNotification(string message);
		void handleDBNotifications();
		static void handleDBNotify(int fd, short event_type, void *ctx)
		{
			FileManager *watcher = reinterpret_cast<FileManager*>(ctx);
			watcher->_impl->handleDBNotifications();
		}
		
		static void handleIgnoreFSNotesOff(int fd, short event_type, void *ctx)
		{
			EventCallbackWrapper *wrapper = reinterpret_cast<EventCallbackWrapper*>(ctx);
			wrapper->impl->ignoreFSNotifications_ = false;
			event_del(wrapper->event);
			delete wrapper;
		}
		
		void ignoreFSNotifications();

	struct EventCallbackWrapper {
		struct event *event;
		RC2::FileManager::Impl *impl;
	};

	class StatException : public runtime_error {
		using runtime_error::runtime_error;
	};
};

void
RC2::FileManager::Impl::cleanup() {
	if (inotifyFd_ != -1)
		close(inotifyFd_);
	if (dbcon_)
		PQfinish(dbcon_);
}

void
RC2::FileManager::Impl::connect(string str, long wspaceId, long sessionRecId) 
{
	wspaceId_ = wspaceId;
	sessionRecId_ = sessionRecId;
	dbcon_ = PQconnectdb(str.c_str());
	//TODO: error checking
	if (NULL == dbcon_) {
		LOG(ERROR) << "PQconnectdb returned NULL" << endl;
		abort();
	} else if (PQstatus(dbcon_) != CONNECTION_OK) {
		LOG(ERROR) << "failed to connect to database: " << PQerrorMessage(dbcon_) << endl;
		abort();
	}
	char msg[255];
	dbFileSource_.initializeSource(dbcon_, wspaceId_);
	snprintf(msg, 255, "where wspaceid = %ld", wspaceId_);
	dbFileSource_.loadFiles(msg);
	sessionImageBatch_ = 0;
}

long
RC2::FileManager::Impl::insertImage(string fname, string imgNumStr)
{
	string filePath = workingDir + "/" + fname;
	size_t size;
	unique_ptr<char[]> buffer = ReadFileBlob(filePath, size);
	if (size < 1) {
		LOG(INFO) << "got image with no data" << endl;
		return 0;
	}
	long imgId = DBLongFromQuery(dbcon_, "select nextval('sessionimage_seq'::regclass)");
	if (imgId <= 0)
		throw FormattedException("failed to get session image id");
	if (sessionImageBatch_ <= 0) {
		stringstream batchq;
		batchq << "select max(batchid) from sessionimage where sessionid = " << sessionRecId_;
		sessionImageBatch_ = DBLongFromQuery(dbcon_, batchq.str().c_str()) + 1;
	}
	stringstream query;
	query << "insert into sessionimage (id,sessionid,batchid,name,imgdata) values (" << imgId 
		<< "," << sessionRecId_ << "," << sessionImageBatch_ << ",'img" << imgId << ".png',$1::bytea)";
	int pformats = 1;
	int pSizes[] = {(int)size};
 	const char *params[] = {buffer.get()};
	DBResult res(PQexecParams(dbcon_, query.str().c_str(), 1, NULL, params,  
		pSizes, &pformats, 1));
	if (!res.commandOK()) {
		LOG(ERROR) << "insert image error:" << res.errorMessage() << endl;
		throw FormattedException("failed to insert image in db: %s", res.errorMessage());
	}
//	LOG(INFO) << "inserted image " << imgId << " of size " << size << endl;
	imageIds_.push_back(imgId);
	ignoreFSNotifications();
	fs::remove(filePath);
	return 0;
}

void RC2::FileManager::Impl::startImageWatch ( string fname, string imgNum, inotify_event* event )
{
	string fullPath = workingDir + "/" + fname;
	int wd = inotify_add_watch(inotifyFd_, fullPath.c_str(), IN_CLOSE_WRITE | IN_CLOSE_NOWRITE);
	if (wd == -1)
		throw FormattedException("inotify_add_warched failed %s", fname.c_str());
	pendingImagesByWatchDesc_.insert(std::make_pair(wd, PendingImage(fname, imgNum, wd)));
}

void RC2::FileManager::Impl::stopImageWatch ( int wd )
{
	inotify_rm_watch(inotifyFd_, wd);
	pendingImagesByWatchDesc_.erase(wd);
}

void RC2::FileManager::Impl::cleanupImageWatch()
{
	for(auto it = pendingImagesByWatchDesc_.begin(); it != pendingImagesByWatchDesc_.end(); ++it) {
		LOG(INFO) << "cleaning up image " << it->second.fileName << endl;
		//check to see if image exists on disk with file size > 0
		fs::path imgPath(workingDir);
		imgPath /= it->second.fileName;
		auto size = fs::file_size(imgPath);
		if (size !=  static_cast<uintmax_t>(-1)) {
			insertImage(it->second.fileName, it->second.imgNumStr);
			//have to manually stopImageWatch so itr isn't mutated while looping
			inotify_rm_watch(inotifyFd_, it->second.wd);
		}
	}
	//clear set
	pendingImagesByWatchDesc_.erase(pendingImagesByWatchDesc_.begin(), pendingImagesByWatchDesc_.end());
}


void
RC2::FileManager::Impl::processDBNotification(string message)
{
	LOG(INFO) << "db notification: " << message << endl;
	const char *msgStr = message.c_str();
	char type = msgStr[0];
	if (!(type == 'i' || type == 'u' || type == 'd') || message.length() < 2) {
		LOG(ERROR) << "bad db notification received:" << message << endl;
		return;
	}
	long fileId = atol(&msgStr[1]);
	try {
		if (type == 'd') {
			try {
				DBFileInfoPtr fobj = dbFileSource_.filesById_.at(fileId);
				//stop notify watch first
				inotify_rm_watch(inotifyFd_, fobj->watchDescriptor);
				dbFileSource_.filesById_.erase(fileId);
				filesByWatchDesc_.erase(fileId);
				fs::remove(workingDir + "/" + fobj->path);
			} catch (out_of_range &ore) {
				//we'll just ignore
				LOG(WARNING) << "got db note to delete " << fileId << 
					" but we dont' have a desc for it:" << ore.what() << endl;
			} catch (exception &ee) {
				//we'll just ignore
				LOG(WARNING) << "XX got db note to delete " << fileId << 
					" but we dont' have a desc for it:" << ee.what() << endl;
			}
		} else {
			//parse wspace and project ids
			long fileWspace=0, fileProject=0;
			istringstream ss(&msgStr[2]);
			string wstr, pstr;
			if (getline(ss, wstr, '/')) {
				fileWspace = atol(wstr.c_str());
			} else {
				throw runtime_error("failed to parse db notification");
			}
			ostringstream query;
			query << " where f.id = " << fileId;
			if (type == 'i') {
				if (fileWspace == wspaceId_) {
					ignoreFSNotifications();
					dbFileSource_.loadFiles(query.str().c_str());
					watchFile(dbFileSource_.filesById_[fileId]);
				}
			} else if (type == 'u') {
				if (dbFileSource_.filesById_.count(fileId) > 0) {
					ignoreFSNotifications();
					dbFileSource_.loadFiles(query.str().c_str());
				}
			}
		}
	} catch (exception &e) {
		LOG(ERROR) << "exception handling db notification: " << e.what() << endl;
	}
}

void
RC2::FileManager::Impl::handleDBNotifications()
{
	ignoreFSNotifications();
	PGnotify *notify;
	PQconsumeInput(dbcon_);
	while((notify = PQnotifies(dbcon_)) != NULL) {
		if (!ignoreDBNotifications_)
			processDBNotification(notify->extra);
		PQfreemem(notify);
	}
}

bool
RC2::FileManager::Impl::executeDBCommand(string cmd)
{
	DBResult res(PQexec(dbcon_, cmd.c_str()));
	if (res.commandOK()) {
		LOG(ERROR) << "sql error executing: " << cmd << " (" 
			<< res.errorMessage() << ")" << endl;
		return false;
	}
	return true;
}

unique_ptr<char[]>
RC2::FileManager::Impl::readFileBlob(DBFileInfoPtr fobj, size_t &size)
{
	string filePath = workingDir + "/" + fobj->path;
	return ReadFileBlob(filePath, size);
}

//this should be called to turn ignoreFSNotes on. It schedules a low priority event
// to turn it off. That way any higher priority events can be processed before turing this flag off
void
RC2::FileManager::Impl::ignoreFSNotifications() 
{
	if (ignoreFSNotifications_) { return; } //already set
	ignoreFSNotifications_ = true;
	struct timeval timeout = {0,5000}; //5 milliseconds
	struct EventCallbackWrapper *wrapper = new EventCallbackWrapper();
	wrapper->impl = this;
	struct event *evt = event_new(eventBase_, -1, 
		EV_TIMEOUT, RC2::FileManager::Impl::handleIgnoreFSNotesOff, wrapper);
	wrapper->event = evt;
	event_priority_set(evt, 2); //so inotify events handled first
	event_add(evt, &timeout);
}

#pragma mark -

void
RC2::FileManager::Impl::setupInotify(FileManager *fm) {
	inotifyFd_ = inotify_init();
	if (inotifyFd_ == -1)
		throw FormattedException("inotify_init() failed for %s", workingDir.c_str());
	rootDir_.wd = inotify_add_watch(inotifyFd_, workingDir.c_str(), 
		IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVE);
	if (rootDir_.wd == -1)
		throw FormattedException("inotify_add_watched failed");
	stat(workingDir.c_str(), &rootDir_.sb);
	for (auto itr=dbFileSource_.filesById_.begin(); itr != dbFileSource_.filesById_.end(); ++itr) {
		try {
			watchFile(itr->second);
		} catch (Impl::StatException &se) {
		}
	}
	
	struct bufferevent *evt = bufferevent_socket_new(eventBase_, inotifyFd_, 0);
	bufferevent_priority_set(evt, 0); //high priority
	bufferevent_setcb(evt, FileManager::Impl::handleInotifyEvent, NULL, NULL, fm);
	bufferevent_enable(evt, EV_READ);
	inotifyEvent_ = evt;
}

#define I_BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

void
RC2::FileManager::Impl::handleInotifyEvent(struct bufferevent *bev)
{
	if (ignoreFSNotifications_)
		return;
	ignoreFSNotifications();
	char buf[I_BUF_LEN];
	size_t numRead = bufferevent_read(bev, buf, I_BUF_LEN);
	char *p;
	for (p=buf; p < buf + numRead; ) {
		struct inotify_event *event = (struct inotify_event*)p;
		int evtype = event->mask & 0xffff; //events are in lower word, flags in upper
		LOG(INFO) << "notify:" << std::hex << evtype << " (" << std::hex << event->mask << ") for " << event->name << endl;
		try {
			if(evtype == IN_CREATE) {
				if (!(event->mask & IN_ISDIR)) { //we don't want these events, they are duplicates
					long newFileId=0;
					string fname = event->name;
					boost::smatch what;
					if (event->name[0] != '.') {
						if (boost::regex_match(fname, what, imgRegex_, boost::match_default)) {
							startImageWatch(fname, what[1], event);
//							newFileId = insertImage(fname, what[1]);
						} else if (manuallyAddedFiles_.find(fname) == manuallyAddedFiles_.end()) {
							LOG(INFO) << "inotify create for " << fname << endl << manuallyAddedFiles_.size() << endl;
							newFileId = dbFileSource_.insertDBFile(fname);
							LOG(INFO) << "inserted s " << newFileId << endl;
						}
						if (newFileId > 0) {
							//need to add a watch for this file
							watchFile(dbFileSource_.filesById_[newFileId]);
						}
					}
				}
			} else if (evtype == IN_CLOSE_WRITE) {
				auto imgItr = pendingImagesByWatchDesc_.find(event->wd);
				if (imgItr != pendingImagesByWatchDesc_.end()) {
					insertImage(imgItr->second.fileName, imgItr->second.imgNumStr);
					stopImageWatch(event->wd);
				} else {
					DBFileInfoPtr fobj = filesByWatchDesc_[event->wd];
					LOG(INFO) << "got close write event for " << fobj->name << endl;
					dbFileSource_.updateDBFile(fobj);
				}
			} else if (evtype == IN_DELETE_SELF) {

				DBFileInfoPtr fobj = filesByWatchDesc_[event->wd];
			LOG(INFO) << "got delete event for " << fobj->name << endl;
				dbFileSource_.removeDBFile(fobj);
				filesByWatchDesc_.erase(fobj->id);
				//discard our records of it
				inotify_rm_watch(inotifyFd_, event->wd);
			}
		} catch(exception &ex) {
			LOG(ERROR) << "exception in inotify code: " << ex.what() << endl;
		}
		//handle event
		p += sizeof(struct inotify_event) + event->len;
	}
}

void 
RC2::FileManager::Impl::watchFile(DBFileInfoPtr file) {
	string fullPath = workingDir + "/" + file->path;
	if (stat(fullPath.c_str(), &file->sb) == -1)
		throw StatException((format("stat failed for watch %s") % fullPath.c_str()).str());
	file->watchDescriptor = inotify_add_watch(inotifyFd_, fullPath.c_str(), 
				IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MODIFY);
	if (file->watchDescriptor == -1)
		throw FormattedException("inotify_add_warched failed %s", file->name.c_str());
	filesByWatchDesc_[file->watchDescriptor] = file;
}


#pragma mark -


RC2::FileManager::FileManager()
	: _impl(new Impl())
{
	_impl->eventBase_ = nullptr;
	_impl->inotifyFd_ = -1;
}

RC2::FileManager::~FileManager()
{
	_impl->cleanup();
}

void
RC2::FileManager::initFileManager(string connectString, int wspaceId, int sessionRecId)
{
	_impl->connect(connectString, wspaceId, sessionRecId);
	_impl->setupInotify(this);
	DBResult listenRes(_impl->dbcon_, "listen rcfile");
	struct event *evt = event_new(_impl->eventBase_, PQsocket(_impl->dbcon_), 
		EV_READ|EV_PERSIST, RC2::FileManager::Impl::handleDBNotify, this);
	event_priority_set(evt, 2); //so inotify events handled first
	event_add(evt, NULL);
}

void RC2::FileManager::suspendNotifyEvents()
{
	if (bufferevent_get_enabled(_impl->inotifyEvent_) != 0)
		throw std::runtime_error("notify events already suspended");
	bufferevent_disable(_impl->inotifyEvent_, EV_READ);
}

void RC2::FileManager::resumeNotifyEvents()
{
	if (bufferevent_get_enabled(_impl->inotifyEvent_) == 0)
		throw std::runtime_error("notify events already enabled");
	bufferevent_disable(_impl->inotifyEvent_, EV_READ);
}

void
RC2::FileManager::setWorkingDir(std::string dir) 
{ 
	_impl->workingDir = dir; 
	_impl->dbFileSource_.setWorkingDir(dir);
}

void
RC2::FileManager::setEventBase(struct event_base *eb)
{
	_impl->eventBase_ = eb;
}

void
RC2::FileManager::resetWatch()
{
	if (_impl->imageIds_.size() > 0) {
		_impl->sessionImageBatch_++;
		LOG(INFO) << "incrementing batch_id:" << _impl->sessionImageBatch_ << endl;
	}
	_impl->imageIds_.erase(_impl->imageIds_.begin(), _impl->imageIds_.end());
	_impl->manuallyAddedFiles_.clear();
}

void
RC2::FileManager::checkWatch(vector<long> &imageIds, long &batchId)
{
	imageIds = _impl->imageIds_;
	batchId = _impl->sessionImageBatch_;
//	_impl->sessionImageBatch_ = 0;
}

void RC2::FileManager::cleanupImageWatch()
{
	_impl->cleanupImageWatch();
}

bool
RC2::FileManager::loadRData()
{
	return _impl->dbFileSource_.loadRData();
}

void
RC2::FileManager::saveRData()
{
	_impl->dbFileSource_.saveRData();
}

long
RC2::FileManager::findOrAddFile(std::string fname)
{
	auto & fileMap = _impl->dbFileSource_.filesById_;
	for (auto itr = fileMap.begin(); itr != fileMap.end(); ++itr) {
		DBFileInfoPtr ptr = itr->second;
		if (0 == fname.compare(ptr->name)) {
			//a match. 
			return ptr->id;
		}
	}
	//need to add the file
	LOG(INFO) << "findOrAddFile adding file " << fname << endl;
	long fid = _impl->dbFileSource_.insertDBFile(fname);
	_impl->manuallyAddedFiles_.insert(fname);
	return fid;
}

string
RC2::FileManager::filePathForId(long fileId)
{
	auto & fileCache = _impl->dbFileSource_.filesById_;
	if (fileCache.count(fileId) != 1) {
		LOG(WARNING) << "filenameForId called with invalid id: " << fileId << endl;
		return "";
	}
	return fileCache[fileId]->path;
}

void
RC2::FileManager::processDBNotification(string message)
{
	_impl->processDBNotification(message);
}
