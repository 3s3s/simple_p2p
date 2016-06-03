#ifndef _SERVER
#define _SERVER
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <iostream>

#ifndef WIN32
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#else
#define	_WIN32_WINNT 0x600
#include <io.h>
#include <Winsock2.h>
#include <WS2TCPIP.H>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <vector>
#include <string>
#include <sstream>
#include <map>
#include <memory>

#ifdef WIN32
#define SET_NONBLOCK(socket) {DWORD dw = true; ioctlsocket(socket, FIONBIO, &dw);}
typedef int	socklen_t;
#else
#define SET_NONBLOCK(socket) if (fcntl( socket, F_SETFL, fcntl( socket, F_GETFL, 0 ) | O_NONBLOCK ) < 0) printf("error in fcntl errno=%i\n", errno);
#define closesocket(socket)  close(socket)
#define Sleep(a) usleep(a*1000)
#define SOCKET	int
#define INVALID_SOCKET			-1
#define WSAEWOULDBLOCK			EWOULDBLOCK
#define WSAGetLastError()		errno
#define S_OK					0
#define _close		close
#define _open		open
#define _lseek		lseek
#define _read		read
#endif

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/sendfile.h>
#define O_BINARY	0
#else
#include "epoll.h"
#include "sendfile.h"
#endif
#include <sys/stat.h>

const time_t g_timeCallbackTimerInterval = 10;

using namespace std;
namespace server
{
	enum MESSAGE {
		I_READY_EPOLL,
		I_ACCEPTED,
		I_READ,
		I_ALL_WROTE,
		I_CALL_TIMER,
		PLEASE_READ,
		PLEASE_WRITE_BUFFER,
		PLEASE_WRITE_FILE,
		PLEASE_STOP
	};

	template<class CLIENT>
	class CServerMessages
	{
	public:
		static MESSAGE MessageProc(SOCKET hSocket, MESSAGE message, shared_ptr<vector<unsigned char>> pvBuffer = nullptr)
		{
			static map<SOCKET, shared_ptr<CLIENT>> mapSocketToClient;

			if (hSocket == INVALID_SOCKET) return message;
			if (message == I_ACCEPTED)
			{
				cout << "I_ACCEPTED socket = " << hSocket << "\n";
				mapSocketToClient[hSocket] = shared_ptr<CLIENT>(new CLIENT);
				return mapSocketToClient[hSocket]->OnAccepted(pvBuffer);
			}

			auto it = mapSocketToClient.find(hSocket);
			if (it == mapSocketToClient.end()) 
				return PLEASE_STOP;

			MESSAGE ret = PLEASE_STOP;
			switch (message) {
				case I_READ:
					cout << "I_READ socket = " << hSocket << "\n";
					ret = it->second->OnRead(pvBuffer);
					break;
				case I_ALL_WROTE:
					cout << "I_ALL_WROTE socket = " << hSocket << "\n";
					ret = it->second->OnWrote(pvBuffer);
					break;
				case I_CALL_TIMER: 
					ret = it->second->OnTimer(pvBuffer);
					break;
				case I_READY_EPOLL: case PLEASE_STOP: case PLEASE_READ: case PLEASE_WRITE_BUFFER: case PLEASE_WRITE_FILE: case I_ACCEPTED: break;
			}
			if (ret == PLEASE_STOP) {
				cout << "erase socket = " << hSocket << "\n";
				mapSocketToClient.erase(hSocket);
			}
			return ret;
		}
	};
	
	template<class CLIENT, class T = CServerMessages<CLIENT>>
	class CServer 
	{
		class CClient
		{
			int m_nSendFile;
			off_t m_nFilePos;

			SOCKET m_hSocket; //Дескриптор клиентского сокета
			int m_nLastSocketError;
			vector<unsigned char> m_vRecvBuffer; //В этом буфере клиент будет хранить принятые данные
			vector<unsigned char> m_vSendBuffer; //В этом буфере клиент будет хранить отправляемые данные
		
			explicit CClient(const CClient &); //Нам не понадобится конструктор копирования для клиентов
		private:
			void CleanAndInit()
			{
				m_nSendFile = -1;
				m_nFilePos = 0;
				m_nLastSocketError = 0;
				m_pvBuffer = shared_ptr<vector<unsigned char>>(new vector<unsigned char>);
				m_stateCurrent = S_ACCEPTED_TCP;
			}
			sockaddr_in m_SocketInfo;
		public:
			CClient(const SOCKET hSocket, const sockaddr_in &info) : m_hSocket(hSocket), m_SocketInfo(info)
			{
				SET_NONBLOCK(hSocket);
				CleanAndInit();
			}
			~CClient()
			{
				if(m_hSocket != INVALID_SOCKET)	closesocket(m_hSocket);
			}

			const int GetPort() const { return m_SocketInfo.sin_port; }
			const string GetHostName() const
			{
				char s[MAX_PATH];
				inet_ntop(AF_INET, (void *)&m_SocketInfo.sin_addr, s, MAX_PATH);

				return s;
			}
		private:
			//Перечисляем все возможные состояния клиента. При желании можно добавлять новые.
			enum STATES { S_ACCEPTED_TCP, S_READING, S_WRITING };
			//Перечисляем коды возврата для функций
			enum RETCODES {	RET_WAIT, RET_READY, RET_ERROR };
			
			STATES m_stateCurrent; //Здесь хранится текущее состояние

			//Функция для установки состояния
			void SetState(const STATES state, struct epoll_event *pCurrentEvent) 
			{
				m_stateCurrent = state;

				pCurrentEvent->events = EPOLLERR | EPOLLHUP;
				if (m_nLastSocketError == WSAEWOULDBLOCK) {
					if (m_stateCurrent == S_READING) pCurrentEvent->events |= EPOLLIN;
					if (m_stateCurrent == S_WRITING) pCurrentEvent->events |= EPOLLOUT;					
					return;
				}
				pCurrentEvent->events |= EPOLLIN | EPOLLOUT;
			}
			shared_ptr<vector<unsigned char>> m_pvBuffer;

			const bool SendMessage(MESSAGE message, struct epoll_event *pCurrentEvent)
			{
				cout << "SendMessage\n";
				switch(T::MessageProc(m_hSocket,  message, m_pvBuffer)) {
					case PLEASE_READ:
						if (message == I_ALL_WROTE)
							CleanAndInit();
						SetState(S_READING, pCurrentEvent);
						return true;
					case PLEASE_WRITE_BUFFER:
						SetState(S_WRITING, pCurrentEvent);
						m_nSendFile = -1;
						m_vSendBuffer = *m_pvBuffer;
						cout << "recv message PLEASE_WRITE_BUFFER\n";
						return true;
					case PLEASE_WRITE_FILE:
						SetState(S_WRITING, pCurrentEvent);
						m_vSendBuffer.clear();
						memcpy(&m_nSendFile, &m_pvBuffer->at(0), m_pvBuffer->size());
						cout << "recv message PLEASE_WRITE_FILE m_nSendFile=" << m_nSendFile << "\n";
						return true;
					case I_READY_EPOLL: case I_ACCEPTED: case I_READ: case I_ALL_WROTE: case PLEASE_STOP: case I_CALL_TIMER: break;
				}
				cout << "SendMessage return false\n";
				return false;
			}
		public:
			//Функция для обработки текущего состояния клиента
			const bool Continue(struct epoll_event *pCurrentEvent)
			{
				if ((m_hSocket == INVALID_SOCKET) || (EPOLLERR == (pCurrentEvent->events & EPOLLERR))) 
					return false;

				switch (m_stateCurrent) {
					case S_ACCEPTED_TCP:
						return SendMessage(I_ACCEPTED, pCurrentEvent);
					case S_READING:
					{
						switch (ContinueRead())	{
							case RET_READY:
								*m_pvBuffer = m_vRecvBuffer;
								return SendMessage(I_READ, pCurrentEvent);
							case RET_ERROR:
								return false;
							default:		return true;
						}
						break;
					}
					case S_WRITING:
					{
						cout << "S_WRITING\n";
						if (RET_ERROR == SendFileTCP(m_nSendFile, &m_nFilePos))		
							return false;

						cout << "check IsAllWrote\n";
						return IsAllWrote() ? SendMessage(I_ALL_WROTE, pCurrentEvent) : true;
					}
					default: 
						return false;
				}
			}
		private:
			int GetLastError(int err) const {return WSAGetLastError();}
			const RETCODES ContinueRead()
			{
				static char szBuffer[4096];
			
				//читаем данные от клиента в буфер
				int err;
				errno = 0;
				err = recv(m_hSocket, szBuffer, 4096, 0);

				m_nLastSocketError = GetLastError(err);

				if (err > 0) {
					//Сохраним прочитанные данные в переменной m_vRecvBuffer
					m_vRecvBuffer.resize(m_vRecvBuffer.size()+(size_t)err);
					move(szBuffer, szBuffer+err, &m_vRecvBuffer[m_vRecvBuffer.size()-err]);		 
					return RET_READY;
				}
			
				m_nLastSocketError = WSAGetLastError();
				if ((err == 0) || ((m_nLastSocketError != WSAEWOULDBLOCK) && (m_nLastSocketError != S_OK)))
					return RET_ERROR;
				
				return RET_WAIT;
			}

			const RETCODES SendFileTCP(const int nFile, off_t *offset)
			{
				if (nFile == -1 || m_vSendBuffer.size()) return ContinueWrite();
				if ((unsigned long long)-1 == (unsigned long long)sendfile(m_hSocket, nFile, offset, 4096)) return RET_ERROR;
				
				m_nLastSocketError = WSAEWOULDBLOCK;
				return RET_WAIT;
			}
			const bool IsAllWrote() const
			{
				if (m_vSendBuffer.size())	return false;
				if (m_nSendFile == -1)		return true;

				struct stat stat_buf;
				if (fstat(m_nSendFile, &stat_buf) == -1)	return true;		

				return (stat_buf.st_size == m_nFilePos)	? true : false;
			}
			const RETCODES ContinueWrite()
			{
				int err;
				errno = 0;
				err = send(m_hSocket, (const char*)&m_vSendBuffer[0], m_vSendBuffer.size(), 0);

				m_nLastSocketError = GetLastError(err);

				if (err > 0) {
					//Если удалось послать все данные, то переходим к следующему состоянию
					if ((size_t)err == m_vSendBuffer.size()) {
						m_vSendBuffer.clear();
						return RET_READY;
					}
					//Если отослали не все данные, то оставим в буфере только то, что еще не послано
					vector<unsigned char> vTemp(m_vSendBuffer.size()-err);
					move(&m_vSendBuffer[err], &m_vSendBuffer[err]+m_vSendBuffer.size()-err, &vTemp[0]);
					m_vSendBuffer = vTemp;
					return RET_WAIT;
				}
				if (((err == 0) || ((m_nLastSocketError != WSAEWOULDBLOCK) && (m_nLastSocketError != S_OK)))) return RET_ERROR;
				return RET_WAIT;
			}	 
		};
		map<SOCKET, shared_ptr<CClient> > m_mapClients;				//Здесь сервер будет хранить всех клиентов
		struct epoll_event m_ListenEventTCP;						//События слушающих сокетов
		vector<struct epoll_event> m_events;						//События клиентских сокетов

		class host_and_port {public: string host; int port;};
		vector<host_and_port> m_vHostsP2P, m_vBlackList;

		explicit CServer(const CServer &); //Нам не понадобится конструктор копирования для сервера
	public:
		CServer(const uint16_t nPortTCP)
		{
#ifndef WIN32
			struct sigaction sa;			
			memset(&sa, 0, sizeof(sa));		
			sa.sa_handler = SIG_IGN;		
			sigaction(SIGPIPE, &sa, NULL);
#else
			WSADATA wsaData;
			if ( WSAStartup( MAKEWORD( 2, 2 ), &wsaData ) != 0 ) {
				cout << "Could not to find usable WinSock in WSAStartup\n";
				return;
			}
#endif
			/* ----------------------------------------------- */
			/* Prepare TCP socket for receiving connections */
			const int nEpoll = epoll_create (1);
			if (nEpoll == -1) {
				cout << "error: epoll_create";
				return;
			}

			InitListenSocket(nEpoll, nPortTCP, m_ListenEventTCP, socket (AF_INET, SOCK_STREAM, 0));

			//m_vHostsP2P.push_back({ "localhost", 8086 });
			m_vHostsP2P.push_back({ "127.0.0.1", 8087 });
			//m_vHostsP2P.push_back({ "localhost", 8088 });

			while(PLEASE_STOP != T::MessageProc(INVALID_SOCKET, I_READY_EPOLL, shared_ptr<vector<unsigned char>>(new vector<unsigned char>))) {
				m_events.clear();
				m_events.resize(m_mapClients.size()+2);
				Callback(nEpoll, epoll_wait(nEpoll, &m_events[0], m_events.size(), 10));
				ContinueP2P(nEpoll);
			}
		}
	private:
		bool IsConnectedOrBlacklisted(const sockaddr_in &client) const
		{
			const bool bIsConnected = [client, this]() -> bool {
				for (const auto connectedHost : m_mapClients)
				{
					if (client.sin_port == connectedHost.second->GetPort())
						return true;
					if (to_string(client.sin_addr.s_addr).find(connectedHost.second->GetHostName()) == 0)
						return true;
				}
				return false;
			}();
			if (bIsConnected) return true;

			const bool bIsBlacklisted = [client, this]() -> bool {
				for (const auto blacklistHost : m_vBlackList)
				{
					if (client.sin_port == blacklistHost.port)
						return true;
					if (to_string(client.sin_addr.s_addr).find(blacklistHost.host) == 0)
						return true;
				}
				return false;
			}();
			if (bIsBlacklisted) return true;

			return false;
		}
		void ContinueP2P(const int nEpoll)
		{
			static time_t timeBlackListClear = time(0);
			if (time(0) - timeBlackListClear > 10)
			{
				m_vBlackList.clear();
				timeBlackListClear = time(0);
			}

			for (const auto p2pHost : m_vHostsP2P)
			{
				sockaddr_in client;
				client.sin_family = AF_INET;
				client.sin_addr.s_addr = inet_addr(p2pHost.host.c_str());
				client.sin_port = htons(p2pHost.port);

				if (IsConnectedOrBlacklisted(client))
					continue;

				const SOCKET ConnectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				if (ConnectSocket == INVALID_SOCKET)
					return;

				SET_NONBLOCK(ConnectSocket);
				
				m_mapClients[ConnectSocket] = shared_ptr<CClient>(new CClient(ConnectSocket, client));

				auto it = m_mapClients.find(ConnectSocket);
				if (it == m_mapClients.end()) return;

				connect(ConnectSocket, (SOCKADDR *)& client, sizeof(client));
				
				//Добавляем нового клиента в epoll
				struct epoll_event ev;
				ev.data.fd = ConnectSocket;
				ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLOUT;
				epoll_ctl(nEpoll, EPOLL_CTL_ADD, it->first, &ev);
			}
		}
		void InitListenSocket(const int nEpoll, const uint16_t nPort, struct epoll_event &eventListen, const SOCKET listen_sd)
		{
			SET_NONBLOCK(listen_sd);
  
			struct sockaddr_in sa_serv;
			memset (&sa_serv, '\0', sizeof(sa_serv));
			sa_serv.sin_family = AF_INET;
			sa_serv.sin_addr	= in4addr_any;
			sa_serv.sin_port	= htons (nPort); /* Server Port number */
  
			if (-1 == ::bind(listen_sd, (struct sockaddr*) &sa_serv, sizeof (sa_serv))) {
				cout << "bind error = " << errno << "\n";
				return;
			}
			/* Receive a TCP connection. */
			listen (listen_sd, SOMAXCONN);

			eventListen.data.fd = listen_sd;
			eventListen.events = EPOLLIN | EPOLLET;
			epoll_ctl (nEpoll, EPOLL_CTL_ADD, listen_sd, &eventListen);
		}
		void AcceptClient(const int nEpoll, const SOCKET hSocketIn)
		{
			struct sockaddr_in sa_cli;  
			socklen_t client_len = sizeof(sa_cli);
			
			SOCKET sd;
			while (INVALID_SOCKET != (sd = accept (hSocketIn, (struct sockaddr*) &sa_cli, (socklen_t *)&client_len))) {
				if (IsConnectedOrBlacklisted(sa_cli))
				{
					closesocket(sd);
					return;
				}
				
				//Добавляем нового клиента в класс сервера
				cout << "Client Accepted\n";
				m_mapClients[sd] = shared_ptr<CClient>(new CClient(sd, sa_cli));
				
				auto it = m_mapClients.find(sd);
				if (it == m_mapClients.end())
				{
					closesocket(sd);
					return;
				}
						
				//Добавляем нового клиента в epoll
				struct epoll_event ev;
				ev.data.fd = sd;
				ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLOUT;
				epoll_ctl (nEpoll, EPOLL_CTL_ADD, it->first, &ev);
			}					
		}
		void DeleteClient(const int nEpoll, const SOCKET hSocket)
		{
			epoll_ctl (nEpoll, EPOLL_CTL_DEL, hSocket, NULL);

			const auto client = m_mapClients[hSocket];
			m_vBlackList.push_back({ client->GetHostName(), client->GetPort() });

			m_mapClients.erase(hSocket);					
			cout << "Delete Client, ClientsCount=" << m_mapClients.size() << "\n";
		}
		void Callback(const int nEpoll, const int nCount)
		{
			for (int i = 0; i < nCount; i++) {
				if (m_events[i].data.fd == m_ListenEventTCP.data.fd)
				{
					AcceptClient(nEpoll, m_ListenEventTCP.data.fd);	//Принимаем коннект от нового клиента
					continue;
				}
				auto it = m_mapClients.find(m_events[i].data.fd);							//Находим клиента по сокету
				if (it == m_mapClients.end()) continue;										//Если не нашли клиента по сокету
				if (!it->second->Continue(&m_events[i])) DeleteClient(nEpoll, it->first);	//Если клиент вернул ошибку
			}

			static time_t tmLastTimerCallback = time(NULL);
			if (time(NULL) - tmLastTimerCallback < g_timeCallbackTimerInterval) return;

			tmLastTimerCallback = time(NULL);

			auto it = m_mapClients.begin();
			while (it != m_mapClients.end())
			{
				SOCKET hSocket = it->first;
				it++;
				if (T::MessageProc(hSocket, I_CALL_TIMER) == PLEASE_STOP) DeleteClient(nEpoll, hSocket); //Клиент попросил об остановке
			}
		}
	};
}

#endif
