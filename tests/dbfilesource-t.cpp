#include <gtest/gtest.h>
#include <string>
#include <iostream>
#include <fstream>
#include <memory>
#include "../src/RC2Logging.h"
#include "../src/DBFileSource.hpp"
#include "common/RC2Utils.hpp"
#include "common/PGDBConnection.hpp"
#define BOOST_NO_CXX11_SCOPED_ENUMS
#include <boost/filesystem.hpp>


using namespace std;
namespace fs = boost::filesystem;

namespace testing {

	class DBSourceTest: public ::testing::Test {
	public:
		RC2::TemporaryDirectory tmpDir;
		RC2::DBFileSource source;
		shared_ptr<RC2::PGDBConnection> db;
		
	protected:
		virtual void SetUp() {
			using namespace g3;
			using namespace RC2;
			std::unique_ptr<LogWorker> logworker{ LogWorker::createLogWorker() };
			auto sinkHandle = logworker->addSink(std2::make_unique<CustomSink>(),
												 &CustomSink::ReceiveLogMessage);
			db = make_shared<RC2::PGDBConnection>();
			db->connect("postgresql://rc2@localhost/rc2test?application_name=unittest&sslmode=disable");
			source.initializeSource(db, 1);
			source.setWorkingDir(tmpDir.getPath());
		}
		
		virtual void TearDown() {
			//nuke the fake .RData we created
			db->executeQuery("delete from rcworkspacedata where id = 1");
			db = nullptr;
		}
	};
	
	TEST_F(DBSourceTest, loadRData)
	{
		string data("foo\nbar\n");
		fs::path path = this->tmpDir.getPath();
		path += "/.RData";
		ofstream ofs(path.c_str());
		ofs << data;
		ofs.close();
		
		source.saveRData();
		fs:remove(path);
		source.loadRData();
		ASSERT_EQ(data.length(), fs::file_size(path));
		
		ifstream ifs(path.c_str());
		string line, text;
		while (getline(ifs, line)) {
			text += line + "\n";
		}
		ifs.close();
		ASSERT_EQ(data, text);
	}

};
