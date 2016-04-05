#include <gtest/gtest.h>
#include <string>
#include <iostream>
#include <queue>
#include <event2/event.h>
#include <glog/logging.h>
#include "common/RC2Utils.hpp"
#include "json.hpp"
#include "../src/RSession.hpp"
#include "../src/RSessionCallbacks.hpp"
#define BOOST_NO_CXX11_SCOPED_ENUMS
#include <boost/filesystem.hpp>

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
	
	static void myevent_logger(int severity, const char *msg) {
		string val(msg);
// 		stderr << val << endl;
	}
	
// int
// intValueFromVariableArray(json &array, string key, string value) {
// 	for (auto itr=array.begin(); itr != array.end(); ++itr) {
// 		auto val = *itr;
// 		if (!val.is_null() && val == value)
// 		
// 		string val = ((json::String)obj[key]).Value();
// 		if (val == value) {
// 			json::Array valArray(obj["value"]);
// 			return ((json::Number)valArray[0]).Value();
// 		}
// 	}
// 	return -1;
// }


class TestingSession : public RSession {
	public:
		using RSession::RSession;
		virtual void	sendJsonToClientSource(std::string json);
		
		bool fileExists(string filename);
		void copyFileToWorkingDirectory(string srcPath);
		void removeAllWorkingFiles();
		virtual void startEventLoop();
		
		void doJson(std::string json);
		void stopLoopWhenEmpty() { event_base_loopexit((struct event_base*)getEventBase(), NULL); }
		
		inline void emptyMessages() { while (!_messages.empty()) _messages.pop(); }
		
		json2 popMessage();
		
		queue<string> _messages;
};

void
TestingSession::startEventLoop()
{
	LOG(INFO) << "starting testing event loop" << endl;
	event_base_loop((struct event_base*)getEventBase(), EVLOOP_NONBLOCK);
}

void
TestingSession::sendJsonToClientSource(std::string json) {
	_messages.push(json);
	cerr << json << endl;
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

void
TestingSession::removeAllWorkingFiles()
{
	fs::path wd(getWorkingDirectory());
	for (fs::directory_iterator end_itr, itr(wd); itr != end_itr; ++itr) {
		remove_all(itr->path());
	}
}

json2
TestingSession::popMessage() {
	string str = _messages.front();
	json2 j = json2::parse(str);
	return j;
}

void
TestingSession::doJson(std::string json) {
	handleJsonCommand(json);
}


namespace testing {

	class SessionTest : public ::testing::Test {
		protected:
			static RSessionCallbacks *callbacks;
			static TestingSession *session;
			
			static void SetUpTestCase() {
//				boost::log::core::get()->set_filter(
//					boost::log::trivial::severity >= (boost::log::trivial::info)
//				);
				callbacks = new RSessionCallbacks();
				session = new TestingSession(callbacks);
				event_set_log_callback(myevent_logger);
				session->prepareForRunLoop();
				session->doJson("{\"msg\":\"open\", \"argument\": \"\", \"wspaceId\":1, \"sessionRecId\":1, \"dbhost\":\"localhost\", \"dbuser\":\"rc2\", \"dbname\":\"rc2test\", \"dbpass\":\"rc2\"}");
				cerr << "SetupTestCase session open\n";
			}
		
			static void TearDownTestCase() {
				try {
					delete session;
					session = NULL;
					delete callbacks;
				} catch (...) {
				}
			}
			
			virtual void SetUp() {
				session->emptyMessages();
			}
			
			virtual void TearDown() {
				session->removeAllWorkingFiles();
			}
	}; 

	RSessionCallbacks* SessionTest::callbacks = NULL;
	TestingSession* SessionTest::session = NULL;

	TEST_F(SessionTest, basicScript)
	{
		session->doJson("{\"msg\":\"execScript\", \"argument\":\"2*2\"}");
		session->startEventLoop();
		struct timespec tm = {0, 5000};
		nanosleep(&tm, NULL);
		session->startEventLoop();
		//		session->stopLoopWhenEmpty();
		ASSERT_EQ(session->_messages.size(), 1); //no execComplete 'cause now after timer and no run loopin tests
		json results1 = session->popMessage();
		ASSERT_TRUE(results1["msg"] == "results");
		ASSERT_TRUE(results1["string"] == "[1] 4\n");
	}

	TEST_F(SessionTest, execFiles)
	{
		session->copyFileToWorkingDirectory("test1.R");
		session->doJson("{\"msg\":\"execFile\", \"argument\":\"test1.R\"}");
		ASSERT_EQ(session->_messages.size(), 2);
		json results = session->popMessage();
		ASSERT_TRUE(results["msg"] == "results");
		ASSERT_TRUE(results["string"] == "\n> x <- 4\n\n> x * x\n[1] 16\n");
	}

	TEST_F(SessionTest, execRMD)
	{
		session->copyFileToWorkingDirectory("test1.Rmd");
		session->doJson("{\"msg\":\"execFile\", \"argument\":\"test1.Rmd\"}");
		ASSERT_EQ(session->_messages.size(), 1);
		json results = session->popMessage();
		ASSERT_TRUE(results["msg"] == "execComplete");
		ASSERT_TRUE(session->fileExists("test1.html"));
	}

	TEST_F(SessionTest, execSweave)
	{
		session->copyFileToWorkingDirectory("test1.Rnw");
		session->doJson("{\"msg\":\"execFile\", \"argument\":\"test1.Rnw\"}");
		ASSERT_EQ(session->_messages.size(), 1);
		json results = session->popMessage();
		ASSERT_TRUE(results["msg"] == "execComplete");
		ASSERT_TRUE(session->fileExists("test1.pdf"));
	}

	TEST_F(SessionTest, genImages)
	{
		session->doJson("{\"msg\":\"execScript\", \"argument\":\"plot(rnorm(11))\"}");
		ASSERT_EQ(session->_messages.size(), 1);
		json results = session->popMessage();
		ASSERT_TRUE(results["msg"] == "execComplete");
		ASSERT_TRUE(session->fileExists("rc2img001.png"));
	}

	TEST_F(SessionTest, singleHelp)
	{
		session->emptyMessages();
		session->doJson("{\"msg\":\"help\", \"argument\":\"lm\"}");
		ASSERT_EQ(session->_messages.size(), 1);
		json results = session->popMessage();
		ASSERT_TRUE(results["msg"] == "help");
		ASSERT_TRUE(results["topic"] == "lm");
	}

/*	TEST_F(SessionTest, multipleHelp)
	{
		session->doJson("{\"msg\":\"help\", \"argument\":\"lm\"}");
		ASSERT_EQ(session->_messages.size(), 1);
		json::Object results1 = session->popMessage();
		ASSERT_TRUE(stringForJsonKey(results1, "msg") == "results");
		ASSERT_TRUE(stringForJsonKey(results1, "helpTopic") == "lm");
	}
*/

	TEST_F(SessionTest, getVariable)
	{
		session->doJson("{\"msg\":\"execScript\", \"argument\":\"testVar<-22\"}");
		session->emptyMessages();
		session->doJson("{\"msg\":\"getVariable\", \"argument\":\"testVar\"}");
		ASSERT_EQ(session->_messages.size(), 1);
		json results = session->popMessage();
		ASSERT_TRUE(results["msg"] == "variablevalue");
		ASSERT_TRUE(results["value"]["value"][0] == 22);
	}

	TEST_F(SessionTest, listVariables)
	{
		session->doJson("{\"msg\":\"execScript\", \"argument\":\"rm(list=ls())\"}");
		session->doJson("{\"msg\":\"execScript\", \"argument\":\"x<-22;y<-11\"}");
		session->emptyMessages();
		session->doJson("{\"msg\":\"listVariables\", \"argument\":\"\", \"watch\":true}");
		ASSERT_EQ(session->_messages.size(), 1);
		json results = session->popMessage();
		auto elems = mapArayToMapMap(results["variables"]["values"]);
		ASSERT_TRUE(results["msg"] == "variableupdate");
		ASSERT_EQ(elems["x"], 22);
		ASSERT_EQ(elems["y"], 11);
	}

};
};


