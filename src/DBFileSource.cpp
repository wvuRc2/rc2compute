#include <fstream>
#include <sstream>
#include <arpa/inet.h>
#include <utime.h>
#include "DBFileSource.hpp"
#include "../common/PostgresUtils.hpp"
#include "../common/FormattedException.hpp"
#define BOOST_NO_CXX11_SCOPED_ENUMS
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include "common/RC2Utils.hpp"
#include "RC2Logging.h"

using namespace std;
using boost::format;
namespace fs = boost::filesystem;

class RC2::DBFileSource::Impl {
	public:
	PGconn *db_;
	long wspaceId_;
	long projId_;
	string workingDir_;
};


RC2::DBFileSource::DBFileSource()
{
}

RC2::DBFileSource::~DBFileSource()
{
}

void
RC2::DBFileSource::initializeSource(PGconn *con, long wsid, long projid)
{
	_impl->db_ = con;
	_impl->wspaceId_ = wsid;
	_impl->projId_ = projid;
}

void
RC2::DBFileSource::setWorkingDir(string workingDir)
{
	_impl->workingDir_ = workingDir;
}

void
RC2::DBFileSource::loadFiles(const char *whereClause, bool isProject)
{
	ostringstream query;
	query << "select f.id::int4, f.version::int4, f.name, extract('epoch' from f.lastmodified)::int4, " 
		"d.bindata from rcfile f join rcfiledata d on f.id = d.id " << whereClause;
	DBResult res(PQexecParams(_impl->db_, query.str().c_str(), 0, NULL, NULL, NULL, NULL, 1));
	ExecStatusType rc = PQresultStatus(res);
	if (res.dataReturned()) {
		int numfiles = PQntuples(res);
		for (int i=0; i < numfiles; i++) {
			uint32_t pid=0, pver=0, lastmod=0;
			string pname;
			char *ptr;
			ptr = PQgetvalue(res, i, 0);
			pid = ntohl(*(uint32_t*)ptr);
			ptr = PQgetvalue(res, i, 1);
			pver = ntohl(*(uint32_t*)ptr);
			pname = PQgetvalue(res, i, 2);
			ptr = PQgetvalue(res, i, 3);
			lastmod = ntohl(*(uint32_t*)ptr);
			int datalen = PQgetlength(res, i, 4);
			char *data = PQgetvalue(res, i, 4);
			DBFileInfoPtr filePtr;
			if (filesById_.count(pid) > 0) {
				filePtr = filesById_.at(pid);
				filePtr->version = pver;
				filePtr->name = pname;
			} else {
				filePtr = DBFileInfoPtr(new DBFileInfo(pid, pver, pname, isProject));
				filesById_.insert(map<long,DBFileInfoPtr>::value_type(pid, filePtr));
			}
			//write data to disk
			fs::path filepath(_impl->workingDir_);
			if (isProject)
				filepath /= "shared";
			filepath /= pname;
			ofstream filest;
			filest.open(filepath.string(), ios::out | ios::trunc | ios::binary);
			filest.write(data, datalen);
			filest.close();
			//set modification time
			struct utimbuf modbuf;
			modbuf.actime = modbuf.modtime = lastmod;
			utime(filepath.c_str(), &modbuf);
		}
	} else {
		LOG(ERROR) << "sql error: " << res.errorMessage() << endl;
	}
}


void
RC2::DBFileSource::insertOrUpdateLocalFile(long fileId, long projId, long wspaceId)
{
	if (_impl->wspaceId_ != wspaceId && projId != _impl->projId_)
		return; //skip this file
	ostringstream where;
	where << "where f.id = " << fileId;
	loadFiles(where.str().c_str(), projId == _impl->projId_);
}

void
RC2::DBFileSource::removeLocalFile(long fileId)
{
	DBFileInfoPtr finfo = filesById_[fileId];
	fs::path fpath(_impl->workingDir_);
	fpath /= finfo->path;
	fs::remove(fpath);
	filesById_.erase(fileId);
//	filesByWatchDesc_.erase(fileId);
}

long
RC2::DBFileSource::insertDBFile(string fname, bool isProjectFile)
{
	LOG(INFO) << "insert file to db not implemented\n";
}

void 
RC2::DBFileSource::updateDBFile(DBFileInfoPtr fobj) 
{
	LOG(INFO) << "update file:" << fobj->name << endl;

	int newVersion = fobj->version + 1;
	string fullPath = _impl->workingDir_ + fobj->path;
	if (stat(fullPath.c_str(), &fobj->sb) == -1)
		throw runtime_error((format("stat failed for %s") % fobj->name).str());
	time_t newMod = fobj->sb.st_mtime;
	size_t newSize=0;
	string filePath = _impl->workingDir_ + "/" + fobj->path;
	unique_ptr<char[]> data = ReadFileBlob(filePath, newSize);

	DBTransaction trans(_impl->db_);
	ostringstream query;
	query << "update rcfile set version = " << newVersion << ", lastmodified = to_timestamp("
		<< newMod << "), filesize = " << newSize << " where id = " << fobj->id;
	DBResult res1(_impl->db_, query.str());
	if (!res1.commandOK()) {
		throw FormattedException("failed to update file %ld: %s", fobj->id, res1.errorMessage());
	}
	query.clear();
	query.seekp(0);
	query << "update rcfile set bindata = $1 where id = " << fobj->id;
	Oid in_oid[] = {1043};
	int pformats[] = {1};
	int pSizes[] = {(int)newSize};
	const char *params[] = {data.get()};
	DBResult res2(PQexecParams(_impl->db_, query.str().c_str(), 1, in_oid, params, 
		pSizes, pformats, 1));
	if (!res2.commandOK()) {
		throw FormattedException("failed to update file %ld: %s", fobj->id, res2.errorMessage());
	}
	DBResult commitRes(trans.commit());
	if (!commitRes.commandOK()) {
		throw FormattedException("failed to commit file updates %ld: %s", fobj->id, commitRes.errorMessage());
	}
	fobj->version = newVersion;
}

void 
RC2::DBFileSource::removeDBFile(DBFileInfoPtr fobj) 
{
	ostringstream query;
	query << "delete from rcfile where id = " << fobj->id;
	DBResult res(PQexec(_impl->db_, query.str().c_str()));
	if (res.commandOK()) {
		filesById_.erase(fobj->id);
	} else {
		LOG(ERROR) << "sql error delting file " << fobj->id << ":" 
			<< res.errorMessage() << endl;
	}
}