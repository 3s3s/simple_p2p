#include "server.h"
#include <unordered_map>
#include <time.h>

#define END_OF_MESSAGE	"\n"

namespace server
{
	class Cp2pClient
	{
		explicit Cp2pClient(Cp2pClient &client);

		enum STATES	{
			S_READING_HEADER,
			S_READING_BODY,
			S_WRITING_HEADER,
			S_WRITING_BODY,
			S_ERROR
		};
		
		STATES m_stateCurrent; //текущее состояние клиента
		time_t m_tmLastSocketTime;	//время для контроля таймаута (5 секунд)

		void SetState(const STATES state)
		{
			m_tmLastSocketTime = time(NULL);
			m_stateCurrent = state;
		}
		const MESSAGE OnReadMessage(const string strMessage, shared_ptr<vector<unsigned char>> pvBuffer)
		{
			//Ответ
			string strResponce = strMessage + " is readed!\n";

			//Запоминаем ответ
			pvBuffer->resize(strResponce.length());
			move(strResponce.c_str(), strResponce.c_str() + strResponce.length(), &pvBuffer->at(0));
			return PLEASE_WRITE_BUFFER;
		}

		const MESSAGE CleanAndInit()
		{
			m_stateCurrent = S_READING_HEADER;
			m_tmLastSocketTime = time(NULL);
			return PLEASE_READ;
		}
	public:
		Cp2pClient() { CleanAndInit(); }

		const MESSAGE OnTimer(shared_ptr<vector<unsigned char>> pvBuffer)
		{
			if (time(NULL) - m_tmLastSocketTime > 50)
				return PLEASE_STOP; //Timeout 5 sec
			return PLEASE_READ;
		}
		const MESSAGE OnAccepted(shared_ptr<vector<unsigned char>> pvBuffer) 
		{ 
			return PLEASE_READ;
		}
		const MESSAGE OnWrote(shared_ptr<vector<unsigned char>> pvBuffer)
		{
			//switch (m_stateCurrent) {
			//case S_WRITING_HEADER:
			//	return PLEASE_WRITE_FILE; //Просим сервер послать браузеру файл с дескриптором m_nSendFile
			//default:
				return PLEASE_STOP;
			//}
		}
		const MESSAGE OnRead(shared_ptr<vector<unsigned char>> pvBuffer) //что-то прочитано из сокета
		{
			switch (m_stateCurrent)
			{
				case S_READING_HEADER:
				{
					//Ищем конец сообщения в прочитанных данных
					const std::string strInputString((const char *)&pvBuffer->at(0));
					if (strInputString.find(END_OF_MESSAGE) == strInputString.npos)
						return PLEASE_READ; //Не нашли конец заголовка, читаем дальше

					switch (OnReadMessage(strInputString.substr(0, strInputString.find(END_OF_MESSAGE) + string(END_OF_MESSAGE).length()), pvBuffer)) {
					case PLEASE_READ:
						SetState(S_READING_BODY);
						return PLEASE_READ;
					case PLEASE_WRITE_BUFFER:
						SetState(S_WRITING_HEADER);
						return PLEASE_WRITE_BUFFER;
					default:
						SetState(S_ERROR);
						return PLEASE_STOP;
					}
				}
				default:
					return PLEASE_STOP;
			}
		}
	};

}
