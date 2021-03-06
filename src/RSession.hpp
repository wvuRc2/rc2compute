#pragma once

#include <string>
#include <memory>
#include <stdint.h>
#include <functional>
#include <boost/noncopyable.hpp>
#include "JsonCommand.hpp"
#include "SessionCommon.hpp"

using std::string;

extern const uint32_t kRSessionMagicNumber;

class RInside;

#define kError_Open_InvalidDir 101
#define kError_Open_CreateDirFailed 102
#define kError_Execfile_InvalidInput 103
#define kError_ExecFile_MarkdownFailed 104
#define kErrfor_UnknownFile 105

typedef std::function<void(string&)> OutputCallback;
//typedef void (^OutputCallback)(string &str);

namespace RC2 {

	class RSessionCallbacks;
	class InputBufferManager;
	class FileManager;
	struct FileInfo;

	class RSession : private boost::noncopyable {

		public:
					RSession(RSessionCallbacks *callbacks);
			virtual ~RSession();
			
			bool 	parseArguments(int argc, char *argv[]);
			void	prepareForRunLoop();
			void	installExitHandler(void(*)(short flags));
			virtual void	startEventLoop();
			virtual void	stopEventLoop();

			RSessionCallbacks* getCallbacks() const { return _callbacks; }
			void setCallbacks(RSessionCallbacks *cbs) { _callbacks = cbs; }

			string getWorkingDirectory() const;

			//unit test subclasses might override
			virtual void	sendJsonToClientSource(string json);

		protected:
			void	consoleCallback(const string &text, bool is_error);
			string	formatStringAsJson(const string &input, bool is_error);
			void	clearFileChanges();
			void	flushOutputBuffer();
			void	sendOutputBufferToClient(bool is_error);
			void	sendTextToClient(string text, bool is_error=false);
			void	handleJsonCommand(string json);
			void	handleCommand(JsonCommand& command);
			void	handleOpenCommand(JsonCommand& command);
			void	handleCloseCommand();
			void	handleSaveEnvCommand();
			void	handleHelpCommand(JsonCommand& command);
			void	handleListVariablesCommand(bool delta, JsonCommand& command);
			void	handleGetVariableCommand(JsonCommand& command);

			void	handleExecuteScript(JsonCommand& command);
			void	executeFile(JsonCommand& command);
			void	executeRMarkdown(string filePath, long fileId, JsonCommand& command);
			void	executeSweave(string filePath, long fileId, JsonCommand& command);

			void	scheduleExecCompleteAcknowledgmenet(JsonCommand& command, int queryId, FileInfo *info=nullptr);
			void	scheduleDelayedCommand(string json);
			static void handleJsonStatic(struct bufferevent *bev, void *ctx);

			//this is likely needed for subclasses (like in a unit test)
			//caller will have to cast to event_base*
			struct event_base*	getEventBase() const;
			ExecuteCallback getExecuteCallback();
			bool loadEnvironment();
			
			FileManager const* getFileManager() const; //should not be deallocated
			void setFileManager(FileManager *manager);

			struct Impl;
			std::unique_ptr<Impl>			_impl;
			RSessionCallbacks*		 		_callbacks;

	};
};
