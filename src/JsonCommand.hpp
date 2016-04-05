#pragma once
#include "json.hpp"

using json2 = nlohmann::json;
namespace RC2 {
	
	enum class CommandType {
		Unknown=-1, Open, Close, ClearFileChanges, ExecScript, ExecFile,
		Help, ListVariables, GetVariable, ToggleWatch
	};
	
	class JsonCommand {
		json2::reference _cmd;
		CommandType _type;
	public:
		
		JsonCommand(json2::reference cmd)
			: _cmd(cmd)
			{
				std::string cmdStr = cmd["msg"];
				if (cmdStr == "open") _type = CommandType::Open;
				if (cmdStr == "close") _type = CommandType::Close;
				if (cmdStr == "clearFileChanges") _type = CommandType::ClearFileChanges;
				if (cmdStr == "execScript") _type = CommandType::ExecScript;
				if (cmdStr == "execFile") _type = CommandType::ExecFile;
				if (cmdStr == "help") _type = CommandType::Help;
				if (cmdStr == "listVariables") _type = CommandType::ListVariables;
				if (cmdStr == "getVariable") _type = CommandType::GetVariable;
				if (cmdStr == "toggleVariableWatch") _type = CommandType::ToggleWatch;
			}
			
			CommandType type() const { return _type; }
			json2::reference raw() const { return _cmd; }
			std::string argument() const { return _cmd["argument"]; }
			std::string startTimeStr() const {
				if ( _cmd["startTime"].is_null()) return "";
				return _cmd["startTime"]; 
			}
			bool watchVariables() const { 
				if (_cmd["watchVariables"].is_null()) return false;
				return _cmd["watchVariables"]; 
			}
			json2::reference clientData() const { return _cmd["clientData"]; }
			json2::string_t valueForKey(std::string key) {
				if (_cmd[key].is_null()) return "";
				try { 
					return _cmd[key];
				} catch (std::out_of_range &oe) {
					return "";
				}
			}
		
	};
	
};
