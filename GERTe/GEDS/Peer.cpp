#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#pragma comment(lib, "Ws2_32.lib")

typedef int socklen_t;
#else
#include <sys/socket.h>
#endif

#include "netty.h"
#include "routeManager.h"
#include "logging.h"
#include <fcntl.h>
#include <map>
#include "Poll.h"
#include "GERTc.h"
#include "NetString.h"
#include "gatewayManager.h"
#include "peerManager.h"

using namespace std;

map<IP, Peer*> peers;

extern Poll peerPoll;

enum Commands : char {
	REGISTERED,
	UNREGISTERED,
	ROUTE,
	RESOLVE,
	UNRESOLVE,
	LINK,
	UNLINK,
	CLOSEPEER,
	QUERY
};

Peer::Peer(void * sock) : Connection(sock, "Peer") {
	SOCKET* newSocket = (SOCKET*)sock;
	sockaddr remotename;
	getpeername(*newSocket, &remotename, (socklen_t*)&iplen);
	sockaddr_in remoteip = *(sockaddr_in*)&remotename;
	id = getKnown(remoteip);
	if (id == nullptr) {
		char err[3] = { 0, 0, 1 }; //STATUS ERROR NOT_AUTHORIZED
		error(err);
		throw 1;
	}
	peers[id->addr] = this;
	log("Peer connected from " + id->addr.stringify());
	process();
};

Peer::~Peer() {
	killAssociated(this);
	peers.erase(this->id->addr);

	peerPoll.remove(*(SOCKET*)sock);

	destroy((SOCKET*)this->sock);
	log("Peer " + this->id->addr.stringify() + " disconnected");
}

Peer::Peer(void * socket, KnownPeer * known) : Connection(socket), id(known) {
	peers[id->addr] = this;
}

void Peer::close() {
	this->transmit(string{ Commands::CLOSEPEER }); //SEND CLOSE REQUEST
	delete this;
}

void Peer::transmit(string data) {
	send(*(SOCKET*)this->sock, data.c_str(), (ULONG)data.length(), 0);
}

void Peer::process() {
	if (state == 0) {
		state = 1;
		transmit(string({ (char)ThisVersion.major, (char)ThisVersion.minor }));
		/*
			* Initial packet
			* MAJOR VERSION
			* MINOR VERSION
			*/
		return;
	}
	UCHAR command = (this->read(1))[1];
	switch (command) {
	case ROUTE: {
		GERTc target = GERTc::extract(this);
		GERTc source = GERTc::extract(this);
		NetString data = NetString::extract(this);
		string cmd = { (char)Commands::ROUTE };
		cmd += target.tostring() + source.tostring() + data.string();
		if (!sendToGateway(target.external, cmd)) {
			string errCmd = { UNREGISTERED };
			errCmd += target.tostring();
			this->transmit(errCmd);
		}
		return;
	}
	case REGISTERED: {
		Address target = Address::extract(this);
		setRoute(target, this);
		return;
	}
	case UNREGISTERED: {
		Address target = Address::extract(this);
		removeRoute(target);
		return;
	}
	case RESOLVE: {
		Address target = Address::extract(this);
		Key key = Key::extract(this);
		Key::add(target, key);
		return;
	}
	case UNRESOLVE: {
		Address target = Address::extract(this);
		Key::remove(target);
		return;
	}
	case LINK: {
		IP target = IP::extract(this);
		Ports ports = Ports::extract(this);
		allow(target, ports);
		return;
	}
	case UNLINK: {
		IP target = IP::extract(this);
		deny(target);
		return;
	}
	case CLOSEPEER: {
		this->transmit(string({ CLOSEPEER }));
		delete this;
		return;
	}
	case QUERY: {
		Address target = Address::extract(this);
		if (Gateway::lookup(target)) {
			string cmd = { REGISTERED };
			cmd += target.tostring();
			sendToGateway(target, cmd);
		}
		else {
			string cmd = { UNREGISTERED };
			cmd += target.tostring();
			this->transmit(cmd);
		}
		return;
	}
	}
}
