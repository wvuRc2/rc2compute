#include <iostream>
#include <thread>
#include <glog/logging.h>
#include "common/RC2Utils.hpp"
#define BOOST_NO_CXX11_SCOPED_ENUMS
#include <boost/filesystem.hpp>
#include "TestingSession.hpp"

using json = nlohmann::json;
using namespace std;
namespace fs = boost::filesystem;

namespace RC2 {

	map<string,int64_t> mapArayToMapMap(json jarray) {
		map<string,int64_t> outMap;
		for(auto& element : jarray) {
			outMap[element["name"]] =  element["value"][0];
		}
		return outMap;
	}
	
	void myevent_logger(int severity, const char *msg) {
		string val(msg);
// 		stderr << val << endl;
	}
	

void TestingSession::startCountdown ( int count )
{
	countDown = count; 
	countingDown = true;
}

void TestingSession::executeDelayedJson ( string msg )
{
	struct DelayJson {
		TestingSession *session;
		string msg;
	};
	DelayJson *arg = new DelayJson();
	arg->session = this;
	arg->msg = msg;
	struct timeval delay = {0, 1};
	event_callback_fn closure = [](int f, short fl, void* arg) { 
		DelayJson *msg = (DelayJson*)arg;
		msg->session->doJson(msg->msg); 
		delete msg;
	};
	struct event *ev = event_new(getEventBase(), -1, 0, closure, arg);
	event_priority_set(ev, 0);
	event_add(ev, &delay);
}


void
TestingSession::sendJsonToClientSource(std::string jsonStr) {
	_messages.push(jsonStr + "\n");
	LOG(INFO) <<"t-json:" <<  jsonStr << " (" << countDown << ")" << endl;
	if (countingDown) {
		countDown--;
		if (countDown <= 0) {
			stopEventLoop();
		}
	}
}

bool
TestingSession::fileExists(string filename)
{
	fs::path fp = getWorkingDirectory();
	fp /= filename;
	return fs::exists(fp);
}

void
TestingSession::copyFileToWorkingDirectory(string srcPath)
{
	fs::path src = GetPathForExecutable(getpid());
	src = src.parent_path();
	src /= srcPath;
	fs::path dest = getWorkingDirectory();
	dest /= src.filename();
	if (!fs::exists(src))
		throw runtime_error("no such file");
	fs::copy_file(src, dest, fs::copy_option::overwrite_if_exists);
}

json2
TestingSession::popMessage() {
	string str = _messages.front();
	_messages.pop();
	json2 j = json2::parse(str);
	return j;
}

void
TestingSession::doJson(std::string json) {
	handleJsonCommand(json);
}

void 
TestingSession::execScript ( string rcode )
{
	string json = "{\"msg\":\"execScript\", \"argument\": \"" + rcode + "\"}";
	handleJsonCommand(json);
}

};

namespace testing {
	RSessionCallbacks* BaseSessionTest::callbacks = NULL;
	TestingSession* BaseSessionTest::session = NULL;
	
};
