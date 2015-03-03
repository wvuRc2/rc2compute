/* 
 * File:   main.cpp
 * Author: mlilback
 *
 * Created on February 26, 2015, 2:47 PM
 */

#include <cstdlib>
#include <unistd.h>
#include "RServer.hpp"
#include "tclap/CmdLine.h"
#include "common/RC2Utils.hpp"

using namespace std;


static void event_callback(evutil_socket_t socket, short events, void *objptr);

RServer::RServer()
{
	_port = 7714;
	struct event_config *config = event_config_new();
	event_config_require_features(config, EV_FEATURE_FDS);
	_eventBase = event_base_new_with_config(config);
	event_config_free(config);
}

RServer::~RServer()
{
	event_base_free(_eventBase);
}

void
RServer::startRunLoop()
{
	evutil_socket_t listener;
	struct sockaddr_in sin;
	struct event *listener_event;
	
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = 0;
	sin.sin_port = htons(_port);
	
	listener = socket(AF_INET, SOCK_STREAM, 0);
	evutil_make_socket_nonblocking(listener);
	int one = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(listener, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
		cerr << "error binding socket " << errno << endl;
		exit(-1);
	}
	if (listen(listener, 16) < 0) {
		cerr << "error listening " << errno << endl;
		exit(-1);
	}
	listener_event = event_new(_eventBase, listener, EV_READ|EV_PERSIST, event_callback, this);
	if (NULL == listener_event) {
		cerr << "failed to create event " << errno << endl;
		exit(-1);
	}
	event_add(listener_event, NULL);
	
	event_base_dispatch(_eventBase);
}

void
RServer::handleEvent(evutil_socket_t listener, short events)
{
	string path = RC2::GetPathForExecutable(getpid());
	string::size_type pos = path.rfind('/');
	path = path.substr(0, pos) + "/rsession";
	
	struct sockaddr_in clientAddr;
	socklen_t clientLen = sizeof(clientAddr);
	int clientSock = accept(listener, (struct sockaddr*)&clientAddr, &clientLen);
	if (clientSock == -1) {
		cerr << "Errorr accepting connection: " << errno << endl;
		return;
	}
	_verbose && cout << "client accepted" << endl;
	int option = 1;
	setsockopt(clientSock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
	char pidstr[16];
	sprintf(pidstr, "%d", clientSock);
	const char *args[5];
	args[0] = "rsession";
	args[1] = "-s";
	args[2] = pidstr;
	args[3] = _verbose ? "-v" : NULL;
	args[4] = NULL;
	int forkResult = fork();
	if (forkResult == 0) {
		execve(path.c_str(), (char *const *)args, environ);
		//only get here if there was an error with execve
		std::cerr << "Error with execve:" << errno << std::endl;
		exit(0);
	} else if (forkResult == -1) {
		std::cerr << "failed to fork:" << errno << std::endl;
		close(clientSock);
	}
}

bool
RServer::parseArgs(int argc, char** argv)
{
	try {
		TCLAP::CmdLine cmdLine("Listen for rcompute connections", ' ', "0.1");
		
		TCLAP::ValueArg<uint32_t> portArg("p", "port", "port to listen on", 
			false, 7714, "port", cmdLine);
		
		TCLAP::SwitchArg switchArg("v", "verbose", "enable logging", cmdLine);
			
		cmdLine.parse(argc, argv);
		_port = portArg.getValue();
		_verbose = switchArg.getValue();
		
	} catch (TCLAP::ArgException &e) {
		std::cerr << "error:" << e.error() << std::endl;
	}
	return true;
}

int 
main(int argc, char** argv) 
{
	RServer server;
	server.parseArgs(argc, argv);
	server.startRunLoop();
    return 0;
}


static void 
event_callback(evutil_socket_t socket, short events, void *objptr)
{
	RServer *server = static_cast<RServer*>(objptr);
	server->handleEvent(socket, events);
}