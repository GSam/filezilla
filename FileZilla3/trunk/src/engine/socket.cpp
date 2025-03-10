#include <wx/defs.h>
#ifdef __WXMSW__
  // MinGW needs this for getaddrinfo
  #if defined(_WIN32_WINNT)
    #if _WIN32_WINNT < 0x0600
      #undef _WIN32_WINNT
      #define _WIN32_WINNT 0x0600
    #endif
  #else
    #define _WIN32_WINNT 0x0600
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#endif
#include <filezilla.h>
#include "socket.h"
#include <errno.h>
#ifndef __WXMSW__
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <fcntl.h>
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #if !defined(MSG_NOSIGNAL) && !defined(SO_NOSIGPIPE)
    #include <signal.h>
  #endif
#endif

// Fixups needed on FreeBSD
#if !defined(EAI_ADDRFAMILY) && defined(EAI_FAMILY)
  #define EAI_ADDRFAMILY EAI_FAMILY
#endif

// Union for strict aliasing-safe casting between
// the different address types
union sockaddr_u
{
	struct sockaddr_storage storage;
	struct sockaddr sockaddr;
	struct sockaddr_in in4;
	struct sockaddr_in6 in6;
};

#define WAIT_CONNECT 0x01
#define WAIT_READ	 0x02
#define WAIT_WRITE	 0x04
#define WAIT_ACCEPT  0x08
#define WAIT_CLOSE	 0x10
#define WAIT_EVENTCOUNT 5

class CSocketThread;
static std::list<CSocketThread*> waiting_socket_threads;

struct socket_event_type;
typedef CEvent<socket_event_type> CInternalSocketEvent;

#ifdef __WXMSW__
static int ConvertMSWErrorCode(int error)
{
	switch (error)
	{
	case WSAECONNREFUSED:
		return ECONNREFUSED;
	case WSAECONNABORTED:
		return ECONNABORTED;
	case WSAEINVAL:
		return EAI_BADFLAGS;
	case WSANO_RECOVERY:
		return EAI_FAIL;
	case WSAEAFNOSUPPORT:
		return EAI_FAMILY;
	case WSA_NOT_ENOUGH_MEMORY:
		return EAI_MEMORY;
	case WSANO_DATA:
		return EAI_NODATA;
	case WSAHOST_NOT_FOUND:
		return EAI_NONAME;
	case WSATYPE_NOT_FOUND:
		return EAI_SERVICE;
	case WSAESOCKTNOSUPPORT:
		return EAI_SOCKTYPE;
	case WSAEWOULDBLOCK:
		return EAGAIN;
	case WSAEMFILE:
		return EMFILE;
	case WSAEINTR:
		return EINTR;
	case WSAEFAULT:
		return EFAULT;
	case WSAEACCES:
		return EACCES;
	case WSAETIMEDOUT:
		return ETIMEDOUT;
	case WSAECONNRESET:
		return ECONNRESET;
	default:
		return error;
	}
}
#endif

#ifdef ERRORCODETEST
class CErrorCodeTest
{
public:
	CErrorCodeTest()
	{
		int errors[] = {
			0
		};
		for (int i = 0; errors[i]; ++i)
		{
#ifdef __WXMSW__
			int code = ConvertMSWErrorCode(errors[i]);
#else
			int code = errors[i];
#endif
			if (CSocket::GetErrorDescription(code).Len() < 15)
				wxMessageBoxEx(CSocket::GetErrorDescription(code));
		}

	}
};
#endif

CSocketEventDispatcher::CSocketEventDispatcher(CEventLoop & event_loop)
	: CEventHandler(event_loop)
{
}

CSocketEventDispatcher::~CSocketEventDispatcher()
{
	for (auto const& pendingEvent : m_pending_events ) {
		delete pendingEvent;
	}
}

void CSocketEventDispatcher::SendEvent(CSocketEvent* evt)
{
	wxASSERT(evt->GetSocketEventHandler());
	{
		wxCriticalSectionLocker lock(m_sync);
		m_pending_events.push_back(evt);
	}

	CEventHandler::SendEvent(CInternalSocketEvent());
}

void CSocketEventDispatcher::RemovePending(const CSocketEventHandler* pHandler)
{
	wxCriticalSectionLocker lock(m_sync);

	auto remaining = m_pending_events.begin();
	for (auto iter = m_pending_events.begin(); iter != m_pending_events.end(); ++iter) {
		if ((*iter)->GetSocketEventHandler() != pHandler) {
			*remaining = *iter;
			++remaining;
		}
		else {
			delete *iter;
		}
	}
	m_pending_events.erase(remaining, m_pending_events.end());
}

void CSocketEventDispatcher::RemovePending(const CSocketEventSource* pSource)
{
	wxCriticalSectionLocker lock(m_sync);

	auto remaining = m_pending_events.begin();
	for (auto iter = m_pending_events.begin(); iter != m_pending_events.end(); ++iter) {
		if ((*iter)->GetSocketEventSource() != pSource) {
			*remaining = *iter;
			++remaining;
		}
		else {
			delete *iter;
		}
	}
	m_pending_events.erase(remaining, m_pending_events.end());
}

void CSocketEventDispatcher::UpdatePending(const CSocketEventHandler* pOldHandler, const CSocketEventSource* pOldSource, CSocketEventHandler* pNewHandler, CSocketEventSource* pNewSource)
{
	wxASSERT(pNewHandler);
	wxCriticalSectionLocker lock(m_sync);

	for (auto const& evt : m_pending_events ) {
		if (evt->GetSocketEventSource() != pOldSource || evt->GetSocketEventHandler() != pOldHandler)
			continue;

		evt->m_pSource = pNewSource;
		evt->m_pSocketEventHandler = pNewHandler;
	}
}

void CSocketEventDispatcher::operator()(CEventBase const&)
{
	m_sync.Enter();
	if (m_pending_events.empty()) {
		m_sync.Leave();
		return;
	}

	CSocketEvent *evt = m_pending_events.front();
	m_pending_events.pop_front();

	m_sync.Leave();

	CSocketEventHandler* pHandler = evt->GetSocketEventHandler();
	wxASSERT(pHandler);
	pHandler->OnSocketEvent(*evt);

	delete evt;
}

CSocketEvent::CSocketEvent(CSocketEventHandler* pSocketEventHandler, CSocketEventSource* pSource, EventType type, const wxChar* data)
	: m_pSource(pSource), m_type(type), m_error(0), m_pSocketEventHandler(pSocketEventHandler)
{
	if (data)
	{
		m_data = new wxChar[wxStrlen(data) + 1];
		wxStrcpy(m_data, data);
	}
	else
		m_data = 0;
}

CSocketEvent::CSocketEvent(CSocketEventHandler* pSocketEventHandler, CSocketEventSource* pSource, EventType type, int error /*=0*/)
	: m_pSource(pSource), m_type(type), m_error(error), m_pSocketEventHandler(pSocketEventHandler)
{
	m_data = 0;
}

CSocketEvent::~CSocketEvent()
{
	delete [] m_data;
}

wxString CSocketEvent::GetData() const
{
	if (!m_data)
		return wxString();

	return m_data;
}

CSocketEventHandler::~CSocketEventHandler()
{
	dispatcher_.RemovePending(this);
}

CSocketEventSource::~CSocketEventSource()
{
	dispatcher_.RemovePending(this);
}

class CSocketThread final : protected wxThread
{
	friend class CSocket;
public:
	CSocketThread()
		: wxThread(wxTHREAD_JOINABLE), m_condition(m_sync)
	{
		m_pSocket = 0;
		m_pHost = 0;
		m_pPort = 0;
#ifdef __WXMSW__
		m_sync_event = WSA_INVALID_EVENT;
#else
		m_pipe[0] = -1;
		m_pipe[1] = -1;
#endif
		m_started = false;
		m_quit = false;
		m_finished = false;

		m_waiting = 0;
		m_triggered = 0;
		m_threadwait = false;

		for (int i = 0; i < WAIT_EVENTCOUNT; ++i) {
			m_triggered_errors[i] = 0;
		}
	}

	virtual ~CSocketThread()
	{
#ifdef __WXMSW__
		if (m_sync_event != WSA_INVALID_EVENT)
			WSACloseEvent(m_sync_event);
#else
		if (m_pipe[0] != -1)
			close(m_pipe[0]);
		if (m_pipe[1] != -1)
			close(m_pipe[1]);
#endif
	}

	void SetSocket(CSocket* pSocket, bool already_locked = false)
	{
		if (!already_locked)
			m_sync.Lock();
		m_pSocket = pSocket;

		delete [] m_pHost;
		m_pHost = 0;

		delete [] m_pPort;
		m_pPort = 0;

		m_waiting = 0;
		if (!already_locked)
			m_sync.Unlock();
	}

	int Connect()
	{
		wxASSERT(m_pSocket);
		delete [] m_pHost;
		delete [] m_pPort;

		const wxWX2MBbuf buf = m_pSocket->m_host.mb_str();
		if (!buf)
		{
			m_pHost = 0;
			m_pPort = 0;
			return EINVAL;
		}
		m_pHost = new char[strlen(buf) + 1];
		strcpy(m_pHost, buf);

		// Connect method of CSocket ensures port is in range
		m_pPort = new char[6];
		sprintf(m_pPort, "%u", m_pSocket->m_port);
		m_pPort[5] = 0;

		Start();

		return 0;
	}

	int Start()
	{
		if (m_started) {
			m_sync.Lock();
			wxASSERT(m_threadwait);
			m_waiting = 0;
			WakeupThread(true);
			m_sync.Unlock();
			return 0;
		}
		m_started = true;
#ifdef __WXMSW__
		if (m_sync_event == WSA_INVALID_EVENT)
			m_sync_event = WSACreateEvent();
		if (m_sync_event == WSA_INVALID_EVENT)
			return 1;
#else
		if (m_pipe[0] == -1) {
			if (pipe(m_pipe))
				return errno;
		}
#endif

		int res = Create();
		if (res != wxTHREAD_NO_ERROR)
			return 1;

		Run();

		return 0;
	}

	// Cancels select or idle wait
	void WakeupThread(bool already_locked)
	{
		if (!already_locked)
			m_sync.Lock();

		if (!m_started || m_finished) {
			if (!already_locked)
				m_sync.Unlock();
			return;
		}

		if (m_threadwait) {
			m_threadwait = false;
			m_condition.Signal();
			if (!already_locked)
				m_sync.Unlock();
			return;
		}

#ifdef __WXMSW__
		WSASetEvent(m_sync_event);
#else
		char tmp = 0;

		int ret;
		do {
			ret = write(m_pipe[1], &tmp, 1);
		} while (ret == -1 && errno == EINTR);
#endif
		if (!already_locked)
			m_sync.Unlock();
	}

protected:

	int TryConnectHost(struct addrinfo *addr)
	{
		if (m_pSocket->m_pEvtHandler) {
			CSocketEvent *evt = new CSocketEvent(m_pSocket->m_pEvtHandler, m_pSocket, CSocketEvent::hostaddress, CSocket::AddressToString(addr->ai_addr, addr->ai_addrlen).c_str());
			m_pSocket->dispatcher_.SendEvent(evt);
		}

		int fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
		if (fd == -1) {
#ifdef __WXMSW__
			int res = ConvertMSWErrorCode(WSAGetLastError());
#else
			int res = errno;
#endif
			if (m_pSocket->m_pEvtHandler) {
				CSocketEvent *evt = new CSocketEvent(m_pSocket->GetEventHandler(), m_pSocket, addr->ai_next ? CSocketEvent::connection_next : CSocketEvent::connection, res);
				m_pSocket->dispatcher_.SendEvent(evt);
			}

			return 0;
		}

#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
		// We do not want SIGPIPE if writing to socket.
		const int value = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(int));
#endif

		CSocket::DoSetFlags(fd, m_pSocket->m_flags, m_pSocket->m_flags);
		CSocket::DoSetBufferSizes(fd, m_pSocket->m_buffer_sizes[0], m_pSocket->m_buffer_sizes[1]);
		CSocket::SetNonblocking(fd);

		int res = connect(fd, addr->ai_addr, addr->ai_addrlen);
		if (res == -1) {
#ifdef __WXMSW__
			// Map to POSIX error codes
			int error = WSAGetLastError();
			if (error == WSAEWOULDBLOCK)
				res = EINPROGRESS;
			else
				res = ConvertMSWErrorCode(WSAGetLastError());
#else
			res = errno;
#endif
		}

		if (res == EINPROGRESS) {

			m_pSocket->m_fd = fd;

			bool wait_successful;
			do {
				wait_successful = DoWait(WAIT_CONNECT);
				if ((m_triggered & WAIT_CONNECT))
					break;
			} while (wait_successful);

			if (!wait_successful) {
#ifdef __WXMSW__
				closesocket(fd);
#else
				close(fd);
#endif
				if (m_pSocket)
					m_pSocket->m_fd = -1;
				return -1;
			}
			m_triggered &= ~WAIT_CONNECT;

			res = m_triggered_errors[0];
		}

		if (res) {
			if (m_pSocket->m_pEvtHandler) {
				CSocketEvent *evt = new CSocketEvent(m_pSocket->GetEventHandler(), m_pSocket, addr->ai_next ? CSocketEvent::connection_next : CSocketEvent::connection, res);
				m_pSocket->dispatcher_.SendEvent(evt);
			}

			m_pSocket->m_fd = -1;
#ifdef __WXMSW__
			closesocket(fd);
#else
			close(fd);
#endif
		}
		else {
			m_pSocket->m_fd = fd;
			m_pSocket->m_state = CSocket::connected;

			if (m_pSocket->m_pEvtHandler) {
				CSocketEvent *evt = new CSocketEvent(m_pSocket->GetEventHandler(), m_pSocket, CSocketEvent::connection, 0);
				m_pSocket->dispatcher_.SendEvent(evt);
			}

			// We're now interested in all the other nice events
			m_waiting |= WAIT_READ | WAIT_WRITE;

			return 1;
		}

		return 0;
	}

	// Only call while locked
	bool DoConnect()
	{
		char* pHost;
		char* pPort;

		if (!m_pHost || !m_pPort) {
			m_pSocket->m_state = CSocket::closed;
			return false;
		}

		pHost = m_pHost;
		m_pHost = 0;
		pPort = m_pPort;
		m_pPort = 0;

		m_sync.Unlock();

		struct addrinfo *addressList = 0;
		struct addrinfo hints = {0};
		hints.ai_family = m_pSocket->m_family;
		hints.ai_socktype = SOCK_STREAM;

		int res = getaddrinfo(pHost, pPort, &hints, &addressList);

		delete [] pHost;
		delete [] pPort;

		if (!Lock()) {
			if (!res && addressList)
				freeaddrinfo(addressList);
			if (m_pSocket)
				m_pSocket->m_state = CSocket::closed;
			return false;
		}

		// If state isn't connecting, Close() was called.
		// If m_pHost is set, Close() was called and Connect()
		// afterwards, state is back at connecting.
		// In either case, we need to abort this connection attempt.
		if (m_pSocket->m_state != CSocket::connecting || m_pHost) {
			if (!res && addressList)
				freeaddrinfo(addressList);
			return false;
		}

		if (res) {
#ifdef __WXMSW__
			res = ConvertMSWErrorCode(res);
#endif

			if (m_pSocket->m_pEvtHandler)
			{
				CSocketEvent *evt = new CSocketEvent(m_pSocket->GetEventHandler(), m_pSocket, CSocketEvent::connection, res);
				m_pSocket->dispatcher_.SendEvent(evt);
			}
			m_pSocket->m_state = CSocket::closed;

			return false;
		}

		for (struct addrinfo *addr = addressList; addr; addr = addr->ai_next) {
			res = TryConnectHost(addr);
			if (res == -1) {
				freeaddrinfo(addressList);
				if (m_pSocket)
					m_pSocket->m_state = CSocket::closed;
				return false;
			}
			else if (res) {
				freeaddrinfo(addressList);
				return true;
			}
		}
		freeaddrinfo(addressList);

		if (m_pSocket->m_pEvtHandler) {
			CSocketEvent *evt = new CSocketEvent(m_pSocket->GetEventHandler(), m_pSocket, CSocketEvent::connection, ECONNABORTED);
			m_pSocket->dispatcher_.SendEvent(evt);
		}
		m_pSocket->m_state = CSocket::closed;

		return false;
	}

	// Obtains lock in all cases.
	// Returns false if thread should quit
	bool Lock()
	{
		m_sync.Lock();
		if (m_quit || !m_pSocket)
			return false;

		return true;
	}

	// Call only while locked
	bool DoWait(int wait)
	{
		m_waiting |= wait;

		for (;;) {
#ifdef __WXMSW__
			int wait_events = FD_CLOSE;
			if (m_waiting & WAIT_CONNECT)
				wait_events |= FD_CONNECT;
			if (m_waiting & WAIT_READ)
				wait_events |= FD_READ;
			if (m_waiting & WAIT_WRITE)
				wait_events |= FD_WRITE;
			if (m_waiting & WAIT_ACCEPT)
				wait_events |= FD_ACCEPT;
			if (m_waiting & WAIT_CLOSE)
				wait_events |= FD_CLOSE;
			WSAEventSelect(m_pSocket->m_fd, m_sync_event, wait_events);
			m_sync.Unlock();
			WSAWaitForMultipleEvents(1, &m_sync_event, false, WSA_INFINITE, false);

			m_sync.Lock();
			if (m_quit || !m_pSocket) {
				return false;
			}

			WSANETWORKEVENTS events;
			int res = WSAEnumNetworkEvents(m_pSocket->m_fd, m_sync_event, &events);
			if (res) {
				res = ConvertMSWErrorCode(WSAGetLastError());
				return false;
			}

			if (m_waiting & WAIT_CONNECT) {
				if (events.lNetworkEvents & FD_CONNECT) {
					m_triggered |= WAIT_CONNECT;
					m_triggered_errors[0] = ConvertMSWErrorCode(events.iErrorCode[FD_CONNECT_BIT]);
					m_waiting &= ~WAIT_CONNECT;
				}
			}
			if (m_waiting & WAIT_READ) {
				if (events.lNetworkEvents & FD_READ) {
					m_triggered |= WAIT_READ;
					m_triggered_errors[1] = ConvertMSWErrorCode(events.iErrorCode[FD_READ_BIT]);
					m_waiting &= ~WAIT_READ;
				}
			}
			if (m_waiting & WAIT_WRITE) {
				if (events.lNetworkEvents & FD_WRITE) {
					m_triggered |= WAIT_WRITE;
					m_triggered_errors[2] = ConvertMSWErrorCode(events.iErrorCode[FD_WRITE_BIT]);
					m_waiting &= ~WAIT_WRITE;
				}
			}
			if (m_waiting & WAIT_ACCEPT) {
				if (events.lNetworkEvents & FD_ACCEPT) {
					m_triggered |= WAIT_ACCEPT;
					m_triggered_errors[3] = ConvertMSWErrorCode(events.iErrorCode[FD_ACCEPT_BIT]);
					m_waiting &= ~WAIT_ACCEPT;
				}
			}
			if (m_waiting & WAIT_CLOSE) {
				if (events.lNetworkEvents & FD_CLOSE) {
					m_triggered |= WAIT_CLOSE;
					m_triggered_errors[4] = ConvertMSWErrorCode(events.iErrorCode[FD_CLOSE_BIT]);
					m_waiting &= ~WAIT_CLOSE;
				}
			}

			if (m_triggered || !m_waiting)
				return true;
#else
			fd_set readfds;
			fd_set writefds;
			FD_ZERO(&readfds);
			FD_ZERO(&writefds);

			FD_SET(m_pipe[0], &readfds);
			if (!(m_waiting & WAIT_CONNECT))
				FD_SET(m_pSocket->m_fd, &readfds);

			if (m_waiting & (WAIT_WRITE | WAIT_CONNECT))
				FD_SET(m_pSocket->m_fd, &writefds);

			int max = wxMax(m_pipe[0], m_pSocket->m_fd) + 1;

			m_sync.Unlock();

			int res = select(max, &readfds, &writefds, 0, 0);

			m_sync.Lock();

			if (res > 0 && FD_ISSET(m_pipe[0], &readfds)) {
				char buffer[100];
				int damn_spurious_warning = read(m_pipe[0], buffer, 100);
				(void)damn_spurious_warning; // We do not care about return value and this is definitely correct!
			}

			if (m_quit || !m_pSocket || m_pSocket->m_fd == -1) {
				return false;
			}

			if (!res)
				continue;
			if (res == -1) {
				res = errno;
				//printf("select failed: %s\n", (const char *)CSocket::GetErrorDescription(res).mb_str());
				//fflush(stdout);

				if (res == EINTR)
					continue;

				return false;
			}

			if (m_waiting & WAIT_CONNECT) {
				if (FD_ISSET(m_pSocket->m_fd, &writefds)) {
					int error;
					socklen_t len = sizeof(error);
					int res = getsockopt(m_pSocket->m_fd, SOL_SOCKET, SO_ERROR, &error, &len);
					if (res)
						error = errno;
					m_triggered |= WAIT_CONNECT;
					m_triggered_errors[0] = error;
					m_waiting &= ~WAIT_CONNECT;
				}
			}
			else if (m_waiting & WAIT_ACCEPT) {
				if (FD_ISSET(m_pSocket->m_fd, &readfds)) {
					m_triggered |= WAIT_ACCEPT;
					m_waiting &= ~WAIT_ACCEPT;
				}
			}
			else if (m_waiting & WAIT_READ) {
				if (FD_ISSET(m_pSocket->m_fd, &readfds)) {
					m_triggered |= WAIT_READ;
					m_waiting &= ~WAIT_READ;
				}
			}
			if (m_waiting & WAIT_WRITE) {
				if (FD_ISSET(m_pSocket->m_fd, &writefds)) {
					m_triggered |= WAIT_WRITE;
					m_waiting &= ~WAIT_WRITE;
				}
			}

			if (m_triggered || !m_waiting)
				return true;
#endif
		}
	}

	void SendEvents()
	{
		if (!m_pSocket || !m_pSocket->m_pEvtHandler)
			return;
		if (m_triggered & WAIT_READ) {
			if (m_pSocket->m_synchronous_read_cb)
				m_pSocket->m_synchronous_read_cb->cb();
			CSocketEvent *evt = new CSocketEvent(m_pSocket->m_pEvtHandler, m_pSocket, CSocketEvent::read, m_triggered_errors[1]);
			m_pSocket->dispatcher_.SendEvent(evt);
			m_triggered &= ~WAIT_READ;
		}
		if (m_triggered & WAIT_WRITE) {
			CSocketEvent *evt = new CSocketEvent(m_pSocket->m_pEvtHandler, m_pSocket, CSocketEvent::write, m_triggered_errors[2]);
			m_pSocket->dispatcher_.SendEvent(evt);
			m_triggered &= ~WAIT_WRITE;
		}
		if (m_triggered & WAIT_ACCEPT) {
			CSocketEvent *evt = new CSocketEvent(m_pSocket->m_pEvtHandler, m_pSocket, CSocketEvent::connection, m_triggered_errors[3]);
			m_pSocket->dispatcher_.SendEvent(evt);
			m_triggered &= ~WAIT_ACCEPT;
		}
		if (m_triggered & WAIT_CLOSE) {
			SendCloseEvent();
		}
	}

	void SendCloseEvent()
	{
		if( !m_pSocket || !m_pSocket->m_pEvtHandler ) {
			return;
		}

		CSocketEvent *evt = 0;
#ifdef __WXMSW__
		// MSDN says this:
		//   FD_CLOSE being posted after all data is read from a socket.
		//   An application should check for remaining data upon receipt
		//   of FD_CLOSE to avoid any possibility of losing data.
		// First half is actually plain wrong.
		char buf;
		if( !m_triggered_errors[4] && recv( m_pSocket->m_fd, &buf, 1, MSG_PEEK ) > 0) {
			if( !(m_waiting & WAIT_READ) ) {
				return;
			}
			evt = new CSocketEvent(m_pSocket->m_pEvtHandler, m_pSocket, CSocketEvent::read, 0);
		}
		else
#endif
		{
			evt = new CSocketEvent(m_pSocket->m_pEvtHandler, m_pSocket, CSocketEvent::close, m_triggered_errors[4]);
			m_triggered &= ~WAIT_CLOSE;
		}
		m_pSocket->dispatcher_.SendEvent(evt);
	}

	// Call only while locked
	bool IdleLoop()
	{
		if (m_quit)
			return false;
		while (!m_pSocket || (!m_waiting && !m_pHost)) {
			m_threadwait = true;
			m_condition.Wait();

			if (m_quit)
				return false;
		}

		return true;
	}

	virtual ExitCode Entry()
	{
		m_sync.Lock();
		for (;;) {
			if (!IdleLoop()) {
				m_finished = true;
				m_sync.Unlock();
				return 0;
			}

			if (m_pSocket->m_state == CSocket::listening) {
				while (IdleLoop()) {
					if (m_pSocket->m_fd == -1) {
						m_waiting = 0;
						break;
					}
					if (!DoWait(0))
						break;
					SendEvents();
				}
			}
			else {
				if (m_pSocket->m_state == CSocket::connecting) {
					if (!DoConnect())
						continue;
				}

#ifdef __WXMSW__
				m_waiting |= WAIT_CLOSE;
				int wait_close = WAIT_CLOSE;
#endif
				while (IdleLoop()) {
					if (m_pSocket->m_fd == -1)
					{
						m_waiting = 0;
						break;
					}
					bool res = DoWait(0);

					if (m_triggered & WAIT_CLOSE && m_pSocket) {
						m_pSocket->m_state = CSocket::closing;
#ifdef __WXMSW__
						wait_close = 0;
#endif
					}

					if (!res)
						break;

					SendEvents();
#ifdef __WXMSW__
					m_waiting |= wait_close;
#endif
				}
			}
		}

		m_finished = true;
		return 0;
	}

	CSocket* m_pSocket;

	char* m_pHost;
	char* m_pPort;

#ifdef __WXMSW__
	// We wait on this using WSAWaitForMultipleEvents
	WSAEVENT m_sync_event;
#else
	// A pipe is used to unblock select
	int m_pipe[2];
#endif

	wxMutex m_sync;
	wxCondition m_condition;

	bool m_started;
	bool m_quit;
	bool m_finished;

	// The socket events we are waiting for
	int m_waiting;

	// The triggered socket events
	int m_triggered;
	int m_triggered_errors[WAIT_EVENTCOUNT];

	// Thread waits for instructions
	bool m_threadwait;

	CCallback* m_synchronous_read_cb;
};

CSocket::CSocket(CSocketEventHandler* pEvtHandler, CSocketEventDispatcher& dispatcher)
	: CSocketEventSource(dispatcher)
	, m_pEvtHandler(pEvtHandler)
{
#ifdef ERRORCODETEST
	CErrorCodeTest test;
#endif
	m_fd = -1;
	m_state = none;
	m_pSocketThread = 0;
	m_flags = 0;

	m_port = 0;
	m_family = AF_UNSPEC;

	m_buffer_sizes[0] = -1;
	m_buffer_sizes[1] = -1;
}

CSocket::~CSocket()
{
	if (m_state != none)
		Close();

	DetachThread();
}

void CSocket::DetachThread()
{
	if (!m_pSocketThread)
		return;

	m_pSocketThread->m_sync.Lock();
	m_pSocketThread->SetSocket(0, true);
	if (m_pSocketThread->m_finished)
	{
		m_pSocketThread->WakeupThread(true);
		m_pSocketThread->m_sync.Unlock();
		m_pSocketThread->Wait();
		delete m_pSocketThread;
	}
	else
	{
		if (!m_pSocketThread->m_started)
		{
			m_pSocketThread->m_sync.Unlock();
			delete m_pSocketThread;
		}
		else
		{
			m_pSocketThread->m_quit = true;
			m_pSocketThread->WakeupThread(true);
			m_pSocketThread->m_sync.Unlock();
			waiting_socket_threads.push_back(m_pSocketThread);
		}
	}
	m_pSocketThread = 0;

	Cleanup(false);
}

int CSocket::Connect(wxString host, unsigned int port, address_family family /*=unsped*/)
{
	if (m_state != none)
		return EISCONN;

	if (port < 1 || port > 65535)
		return EINVAL;

	switch (family)
	{
	case unspec:
		m_family = AF_UNSPEC;
		break;
	case ipv4:
		m_family = AF_INET;
		break;
	case ipv6:
		m_family = AF_INET6;
		break;
	default:
		return EINVAL;
	}

	if (m_pSocketThread && m_pSocketThread->m_started)
	{
		m_pSocketThread->m_sync.Lock();
		if (!m_pSocketThread->m_threadwait)
		{
			m_pSocketThread->WakeupThread(true);
			m_pSocketThread->m_sync.Unlock();
			// Wait a small amount of time
			wxMilliSleep(100);

			m_pSocketThread->m_sync.Lock();
			if (!m_pSocketThread->m_threadwait)
			{
				// Inside a blocking call, e.g. getaddrinfo
				m_pSocketThread->m_sync.Unlock();
				DetachThread();
			}
			else
				m_pSocketThread->m_sync.Unlock();
		}
		else
			m_pSocketThread->m_sync.Unlock();
	}
	if (!m_pSocketThread)
	{
		m_pSocketThread = new CSocketThread();
		m_pSocketThread->SetSocket(this);
	}

	m_state = connecting;

	m_host = host;
	m_port = port;
	int res = m_pSocketThread->Connect();
	if (res)
	{
		m_state = none;
		delete m_pSocketThread;
		m_pSocketThread = 0;
		return res;
	}

	return EINPROGRESS;
}

void CSocket::SetEventHandler(CSocketEventHandler* pEvtHandler)
{
	if (m_pSocketThread)
		m_pSocketThread->m_sync.Lock();

	if (!pEvtHandler) {
		dispatcher_.RemovePending(m_pEvtHandler);
	}
	else {
		if (m_pEvtHandler)
			dispatcher_.UpdatePending(m_pEvtHandler, this, pEvtHandler, this);
	}

	m_pEvtHandler = pEvtHandler;

	if (m_pSocketThread) {
		if (pEvtHandler && m_state == connected) {
#ifdef __WXMSW__
			// If a graceful shutdown is going on in background already,
			// no further events are recorded. Send out events we're not
			// waiting for (i.e. they got triggered already) manually.

			if (!(m_pSocketThread->m_waiting & WAIT_WRITE)) {
				CSocketEvent *evt = new CSocketEvent(pEvtHandler, this, CSocketEvent::write, 0);
				dispatcher_.SendEvent(evt);
			}

			CSocketEvent *evt = new CSocketEvent(pEvtHandler, this, CSocketEvent::read, 0);
			dispatcher_.SendEvent(evt);
			if (m_pSocketThread->m_waiting & WAIT_WRITE) {
				m_pSocketThread->m_waiting &= ~WAIT_READ;
				m_pSocketThread->WakeupThread(true);
			}
#else
			m_pSocketThread->m_waiting |= WAIT_READ | WAIT_WRITE;
			m_pSocketThread->WakeupThread(true);
#endif
		}
		else if (pEvtHandler && m_state == closing) {
			m_pSocketThread->SendEvents();
		}
		m_pSocketThread->m_sync.Unlock();
	}
}

#define ERRORDECL(c, desc) { c, _T(#c), desc },

struct Error_table
{
	int code;
	const wxChar* const name;
	const wxChar* const description;
};

static struct Error_table error_table[] =
{
	ERRORDECL(EACCES, TRANSLATE_T("Permission denied"))
	ERRORDECL(EADDRINUSE, TRANSLATE_T("Local address in use"))
	ERRORDECL(EAFNOSUPPORT, TRANSLATE_T("The specified address family is not supported"))
	ERRORDECL(EINPROGRESS, TRANSLATE_T("Operation in progress"))
	ERRORDECL(EINVAL, TRANSLATE_T("Invalid argument passed"))
	ERRORDECL(EMFILE, TRANSLATE_T("Process file table overflow"))
	ERRORDECL(ENFILE, TRANSLATE_T("System limit of open files exceeded"))
	ERRORDECL(ENOBUFS, TRANSLATE_T("Out of memory"))
	ERRORDECL(ENOMEM, TRANSLATE_T("Out of memory"))
	ERRORDECL(EPERM, TRANSLATE_T("Permission denied"))
	ERRORDECL(EPROTONOSUPPORT, TRANSLATE_T("Protocol not supported"))
	ERRORDECL(EAGAIN, TRANSLATE_T("Resource temporarily unavailable"))
	ERRORDECL(EALREADY, TRANSLATE_T("Operation already in progress"))
	ERRORDECL(EBADF, TRANSLATE_T("Bad file descriptor"))
	ERRORDECL(ECONNREFUSED, TRANSLATE_T("Connection refused by server"))
	ERRORDECL(EFAULT, TRANSLATE_T("Socket address outside address space"))
	ERRORDECL(EINTR, TRANSLATE_T("Interrupted by signal"))
	ERRORDECL(EISCONN, TRANSLATE_T("Socket already connected"))
	ERRORDECL(ENETUNREACH, TRANSLATE_T("Network unreachable"))
	ERRORDECL(ENOTSOCK, TRANSLATE_T("File descriptor not a socket"))
	ERRORDECL(ETIMEDOUT, TRANSLATE_T("Connection attempt timed out"))
	ERRORDECL(EHOSTUNREACH, TRANSLATE_T("No route to host"))
	ERRORDECL(ENOTCONN, TRANSLATE_T("Socket not connected"))
	ERRORDECL(ENETRESET, TRANSLATE_T("Connection reset by network"))
	ERRORDECL(EOPNOTSUPP, TRANSLATE_T("Operation not supported"))
	ERRORDECL(ESHUTDOWN, TRANSLATE_T("Socket has been shut down"))
	ERRORDECL(EMSGSIZE, TRANSLATE_T("Message too large"))
	ERRORDECL(ECONNABORTED, TRANSLATE_T("Connection aborted"))
	ERRORDECL(ECONNRESET, TRANSLATE_T("Connection reset by peer"))
	ERRORDECL(EPIPE, TRANSLATE_T("Local endpoint has been closed"))

	// Getaddrinfo related
#ifndef __WXMSW__
	ERRORDECL(EAI_ADDRFAMILY, TRANSLATE_T("Network host does not have any network addresses in the requested address family"))
#endif
	ERRORDECL(EAI_AGAIN, TRANSLATE_T("Temporary failure in name resolution"))
	ERRORDECL(EAI_BADFLAGS, TRANSLATE_T("Invalid value for ai_flags"))
#ifdef EAI_BADHINTS
	ERRORDECL(EAI_BADHINTS, TRANSLATE_T("Invalid value for hints"))
#endif
	ERRORDECL(EAI_FAIL, TRANSLATE_T("Nonrecoverable failure in name resolution"))
	ERRORDECL(EAI_FAMILY, TRANSLATE_T("The ai_family member is not supported"))
	ERRORDECL(EAI_MEMORY, TRANSLATE_T("Memory allocation failure"))
#ifdef EAI_NODATA
	ERRORDECL(EAI_NODATA, TRANSLATE_T("No address associated with nodename"))
#endif
	ERRORDECL(EAI_NONAME, TRANSLATE_T("Neither nodename nor servname provided, or not known"))
#ifdef EAI_OVERFLOW
	ERRORDECL(EAI_OVERFLOW, TRANSLATE_T("Argument buffer overflow"))
#endif
#ifdef EAI_PROTOCOL
	ERRORDECL(EAI_PROTOCOL, TRANSLATE_T("Resolved protocol is unknown"))
#endif
	ERRORDECL(EAI_SERVICE, TRANSLATE_T("The servname parameter is not supported for ai_socktype"))
	ERRORDECL(EAI_SOCKTYPE, TRANSLATE_T("The ai_socktype member is not supported"))
#ifndef __WXMSW__
	ERRORDECL(EAI_SYSTEM, TRANSLATE_T("Other system error"))
#endif

	// Codes that have no POSIX equivalence
#ifdef __WXMSW__
	ERRORDECL(WSANOTINITIALISED, TRANSLATE_T("Not initialized, need to call WSAStartup"))
	ERRORDECL(WSAENETDOWN, TRANSLATE_T("System's network subsystem has failed"))
	ERRORDECL(WSAEPROTOTYPE, TRANSLATE_T("Protocol not supported on given socket type"))
	ERRORDECL(WSAESOCKTNOSUPPORT, TRANSLATE_T("Socket type not supported for address family"))
	ERRORDECL(WSAEADDRNOTAVAIL, TRANSLATE_T("Cannot assign requested address"))
	ERRORDECL(ERROR_NETNAME_DELETED, TRANSLATE_T("The specified network name is no longer available"))
#endif
	{ 0, 0, 0 }
};

wxString CSocket::GetErrorString(int error)
{
	for (int i = 0; error_table[i].code; ++i)
	{
		if (error != error_table[i].code)
			continue;

		return error_table[i].name;
	}

	return wxString::Format(_T("%d"), error);
}

wxString CSocket::GetErrorDescription(int error)
{
	for (int i = 0; error_table[i].code; ++i)
	{
		if (error != error_table[i].code)
			continue;

		return wxString(error_table[i].name) + _T(" - ") + wxGetTranslation(error_table[i].description);
	}

	return wxString::Format(_T("%d"), error);
}

int CSocket::Close()
{
	int fd;
	if (m_pSocketThread)
	{
		m_pSocketThread->m_sync.Lock();
		fd = m_fd;
		m_fd = -1;
		delete [] m_pSocketThread->m_pHost;
		m_pSocketThread->m_pHost = 0;
		delete [] m_pSocketThread->m_pPort;
		m_pSocketThread->m_pPort = 0;
		if (!m_pSocketThread->m_threadwait)
			m_pSocketThread->WakeupThread(true);
	}
	else
	{
		fd = m_fd;
		m_fd = -1;
	}

	if (fd != -1)
	{
#ifdef __WXMSW__
		closesocket(fd);
#else
		close(fd);
#endif
	}

	m_state = none;

	if (m_pSocketThread) {
		m_pSocketThread->m_triggered = 0;
		for (int i = 0; i < WAIT_EVENTCOUNT; ++i) {
			m_pSocketThread->m_triggered_errors[i] = 0;
		}
		m_pSocketThread->m_sync.Unlock();
	}

	if (m_pEvtHandler)
		dispatcher_.RemovePending(m_pEvtHandler);

	return 0;
}

CSocket::SocketState CSocket::GetState()
{
	SocketState state;
	if (m_pSocketThread)
		m_pSocketThread->m_sync.Lock();
	state = m_state;
	if (m_pSocketThread)
		m_pSocketThread->m_sync.Unlock();

	return state;
}

bool CSocket::Cleanup(bool force)
{
	std::list<CSocketThread*>::iterator iter = waiting_socket_threads.begin();
	while (iter != waiting_socket_threads.end())
	{
		std::list<CSocketThread*>::iterator current = iter++;
		CSocketThread* pThread = *current;
		pThread->m_sync.Lock();
		if (!force && !pThread->m_finished)
		{
			pThread->m_sync.Unlock();
			continue;
		}
		pThread->m_sync.Unlock();

		pThread->Wait(wxTHREAD_WAIT_BLOCK);
		delete pThread;
		waiting_socket_threads.erase(current);
	}

	return false;
}

int CSocket::Read(void* buffer, unsigned int size, int& error)
{
	int res = recv(m_fd, (char*)buffer, size, 0);

	if (res == -1)
	{
#ifdef __WXMSW__
		error = ConvertMSWErrorCode(WSAGetLastError());
#else
		error = errno;
#endif
		if (error == EAGAIN) {
			if (m_pSocketThread) {
				m_pSocketThread->m_sync.Lock();
				if (!(m_pSocketThread->m_waiting & WAIT_READ)) {
					m_pSocketThread->m_waiting |= WAIT_READ;
					m_pSocketThread->WakeupThread(true);
				}
				m_pSocketThread->m_sync.Unlock();
			}
		}
	}
	else
		error = 0;

	return res;
}

int CSocket::Peek(void* buffer, unsigned int size, int& error)
{
	int res = recv(m_fd, (char*)buffer, size, MSG_PEEK);

	if (res == -1)
	{
#ifdef __WXMSW__
		error = ConvertMSWErrorCode(WSAGetLastError());
#else
		error = errno;
#endif
	}
	else
		error = 0;

	return res;
}

int CSocket::Write(const void* buffer, unsigned int size, int& error)
{
#ifdef MSG_NOSIGNAL
	const int flags = MSG_NOSIGNAL;
#else
	const int flags = 0;

#if !defined(SO_NOSIGPIPE) && !defined(__WXMSW__)
	// Some systems have neither. Need to block signal
	struct sigaction old_action;
	struct sigaction action = {0};
	action.sa_handler = SIG_IGN;
	int signal_set = sigaction(SIGPIPE, &action, &old_action);
#endif

#endif

	int res = send(m_fd, (const char*)buffer, size, flags);

#if !defined(MSG_NOSIGNAL) && !defined(SO_NOSIGPIPE) && !defined(__WXMSW__)
	// Restore previous signal handler
	if (!signal_set)
		sigaction(SIGPIPE, &old_action, 0);
#endif

	if (res == -1)
	{
#ifdef __WXMSW__
		error = ConvertMSWErrorCode(WSAGetLastError());
#else
		error = errno;
#endif
		if (error == EAGAIN)
		{
			if (m_pSocketThread)
			{
				m_pSocketThread->m_sync.Lock();
				if (!(m_pSocketThread->m_waiting & WAIT_WRITE))
				{
					m_pSocketThread->m_waiting |= WAIT_WRITE;
					m_pSocketThread->WakeupThread(true);
				}
				m_pSocketThread->m_sync.Unlock();
			}
		}
	}
	else
		error = 0;

	return res;
}

wxString CSocket::AddressToString(const struct sockaddr* addr, int addr_len, bool with_port /*=true*/, bool strip_zone_index/*=false*/)
{
	char hostbuf[NI_MAXHOST];
	char portbuf[NI_MAXSERV];

	int res = getnameinfo(addr, addr_len, hostbuf, NI_MAXHOST, portbuf, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);
	if (res) // Should never fail
		return wxString();

	wxString host = wxString(hostbuf, wxConvLibc);
	wxString port = wxString(portbuf, wxConvLibc);

	// IPv6 uses colons as separator, need to enclose address
	// to avoid ambiguity if also showing port
	if (addr->sa_family == AF_INET6)
	{
		if (strip_zone_index)
		{
			int pos = host.Find('%');
			if (pos != -1)
				host.Truncate(pos);
		}
		if (with_port)
			host = _T("[") + host + _T("]");
	}

	if (with_port)
		return host + _T(":") + port;
	else
		return host;
}

wxString CSocket::GetLocalIP(bool strip_zone_index /*=false*/) const
{
	struct sockaddr_storage addr;
	socklen_t addr_len = sizeof(addr);
	int res = getsockname(m_fd, (sockaddr*)&addr, &addr_len);
	if (res)
		return wxString();

	return AddressToString((sockaddr *)&addr, addr_len, false, strip_zone_index);
}

wxString CSocket::GetPeerIP(bool strip_zone_index /*=false*/) const
{
	struct sockaddr_storage addr;
	socklen_t addr_len = sizeof(addr);
	int res = getpeername(m_fd, (sockaddr*)&addr, &addr_len);
	if (res)
		return wxString();

	return AddressToString((sockaddr *)&addr, addr_len, false, strip_zone_index);
}

CSocket::address_family CSocket::GetAddressFamily() const
{
	sockaddr_u addr;
	socklen_t addr_len = sizeof(addr);
	int res = getsockname(m_fd, &addr.sockaddr, &addr_len);
	if (res)
		return unspec;

	switch (addr.sockaddr.sa_family)
	{
	case AF_INET:
		return ipv4;
	case AF_INET6:
		return ipv6;
	default:
		return unspec;
	}
}

int CSocket::Listen(address_family family, int port /*=0*/)
{
	if (m_state != none)
		return EALREADY;

	if (port < 0 || port > 65535)
		return EINVAL;

	switch (family)
	{
	case unspec:
		m_family = AF_UNSPEC;
		break;
	case ipv4:
		m_family = AF_INET;
		break;
	case ipv6:
		m_family = AF_INET6;
		break;
	default:
		return EINVAL;
	}

	{
		struct addrinfo hints = {0};
		hints.ai_family = m_family;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
#ifdef AI_NUMERICSERV
		// Some systems like Windows or OS X don't know AI_NUMERICSERV.
		hints.ai_flags |= AI_NUMERICSERV;
#endif

		char portstring[6];
		sprintf(portstring, "%d", port);

		struct addrinfo* addressList = 0;
		int res = getaddrinfo(0, portstring, &hints, &addressList);

		if (res)
		{
#ifdef __WXMSW__
			return ConvertMSWErrorCode(res);
#else
			return res;
#endif
		}

		for (struct addrinfo* addr = addressList; addr; addr = addr->ai_next)
		{
			m_fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
#ifdef __WXMSW__
			res = ConvertMSWErrorCode(WSAGetLastError());
#else
			res = errno;
#endif
			if (m_fd == -1)
				continue;

			SetNonblocking(m_fd);

			res = bind(m_fd, addr->ai_addr, addr->ai_addrlen);
			if (!res)
				break;
#ifdef __WXMSW__
			res = ConvertMSWErrorCode(WSAGetLastError());
			closesocket(m_fd);
#else
			res = errno;
			close(m_fd);
#endif
			m_fd = -1;
		}
		freeaddrinfo(addressList);
		if (m_fd == -1)
			return res;
	}

	int res = listen(m_fd, 1);
	if (res)
	{
#ifdef __WXMSW__
		res = ConvertMSWErrorCode(res);
		closesocket(m_fd);
#else
		close(m_fd);
#endif
		m_fd = -1;
		return res;
	}

	m_state = listening;

	m_pSocketThread = new CSocketThread();
	m_pSocketThread->SetSocket(this);

	m_pSocketThread->m_waiting = WAIT_ACCEPT;

	m_pSocketThread->Start();

	return 0;
}

int CSocket::GetLocalPort(int& error)
{
	sockaddr_u addr;
	socklen_t addr_len = sizeof(addr);
	error = getsockname(m_fd, &addr.sockaddr, &addr_len);
	if (error)
	{
#ifdef __WXMSW__
		error = ConvertMSWErrorCode(error);
#endif
		return -1;
	}

	if (addr.storage.ss_family == AF_INET)
		return ntohs(addr.in4.sin_port);
	else if (addr.storage.ss_family == AF_INET6)
		return ntohs(addr.in6.sin6_port);

	error = EINVAL;
	return -1;
}

int CSocket::GetRemotePort(int& error)
{
	sockaddr_u addr;
	socklen_t addr_len = sizeof(addr);
	error = getpeername(m_fd, &addr.sockaddr, &addr_len);
	if (error)
	{
#ifdef __WXMSW__
		error = ConvertMSWErrorCode(error);
#endif
		return -1;
	}

	if (addr.storage.ss_family == AF_INET)
		return ntohs(addr.in4.sin_port);
	else if (addr.storage.ss_family == AF_INET6)
		return ntohs(addr.in6.sin6_port);

	error = EINVAL;
	return -1;
}

CSocket* CSocket::Accept(int &error)
{
	if (m_pSocketThread)
	{
		m_pSocketThread->m_sync.Lock();
		m_pSocketThread->m_waiting |= WAIT_ACCEPT;
		m_pSocketThread->WakeupThread(true);
		m_pSocketThread->m_sync.Unlock();
	}
	int fd = accept(m_fd, 0, 0);
	if (fd == -1)
	{
#ifdef __WXMSW__
		error = ConvertMSWErrorCode(WSAGetLastError());
#else
		error = errno;
#endif
		return 0;
	}

#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
	// We do not want SIGPIPE if writing to socket.
	const int value = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(int));
#endif

	SetNonblocking(fd);

	DoSetBufferSizes(fd, m_buffer_sizes[0], m_buffer_sizes[1]);

	CSocket* pSocket = new CSocket(0, dispatcher_);
	pSocket->m_state = connected;
	pSocket->m_fd = fd;
	pSocket->m_pSocketThread = new CSocketThread();
	pSocket->m_pSocketThread->SetSocket(pSocket);
	pSocket->m_pSocketThread->m_waiting = WAIT_READ | WAIT_WRITE;
	pSocket->m_pSocketThread->Start();

	return pSocket;
}

int CSocket::SetNonblocking(int fd)
{
	// Set socket to non-blocking.
#ifdef __WXMSW__
	unsigned long nonblock = 1;
	int res = ioctlsocket(fd, FIONBIO, &nonblock);
	if (!res)
		return 0;
	else
		return ConvertMSWErrorCode(WSAGetLastError());
#else
	int flags = fcntl(fd, F_GETFL);
	if (flags == -1)
		return errno;
	int res = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (res == -1)
		return errno;
	return 0;
#endif
}

void CSocket::SetFlags(int flags)
{
	if (m_pSocketThread)
		m_pSocketThread->m_sync.Lock();

	if (m_fd != -1)
		DoSetFlags(m_fd, flags, flags ^ m_flags);
	m_flags = flags;

	if (m_pSocketThread)
		m_pSocketThread->m_sync.Unlock();
}

int CSocket::DoSetFlags(int fd, int flags, int flags_mask)
{
	if (flags_mask & flag_nodelay)
	{
		const int value = (flags & flag_nodelay) ? 1 : 0;
		int res = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&value, sizeof(value));
		if (res != 0)
#ifdef __WXMSW__
			return ConvertMSWErrorCode(WSAGetLastError());
#else
			return errno;
#endif
	}
	if (flags_mask & flag_keepalive)
	{
		const int value = (flags & flag_keepalive) ? 1 : 0;
		int res = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&value, sizeof(value));
		if (res != 0)
#ifdef __WXMSW__
			return ConvertMSWErrorCode(WSAGetLastError());
#else
			return errno;
#endif
	}

	return 0;
}

void CSocket::SetBufferSizes(int size_read, int size_write)
{
	if (m_pSocketThread)
		m_pSocketThread->m_sync.Lock();

	m_buffer_sizes[0] = size_read;
	m_buffer_sizes[1] = size_write;

	if (m_fd != -1)
		DoSetBufferSizes(m_fd, size_read, size_write);

	if (m_pSocketThread)
		m_pSocketThread->m_sync.Unlock();
}

int CSocket::DoSetBufferSizes(int fd, int size_read, int size_write)
{
	if (size_read != -1)
	{
		int res = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&size_read, sizeof(size_read));
		if (res != 0)
#ifdef __WXMSW__
			return ConvertMSWErrorCode(WSAGetLastError());
#else
			return errno;
#endif
	}

	if (size_write != -1)
	{
		int res = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&size_write, sizeof(size_write));
		if (res != 0)
#ifdef __WXMSW__
			return ConvertMSWErrorCode(WSAGetLastError());
#else
			return errno;
#endif
	}

	return 0;
}

void CSocket::SetSynchronousReadCallback(CCallback* cb)
{
	if (m_pSocketThread)
		m_pSocketThread->m_sync.Lock();

	m_synchronous_read_cb = cb;

	if (m_pSocketThread)
		m_pSocketThread->m_sync.Unlock();
}

wxString CSocket::GetPeerHost() const
{
	return m_host;
}
