#ifndef __SOCKET_H__
#define __SOCKET_H__

#include "event_handler.h"

// IPv6 capable, non-blocking socket class for use with wxWidgets.
// Error codes are the same as used by the POSIX socket functions,
// see 'man 2 socket', 'man 2 connect', ...

class CSocketEventHandler;
class CSocketEventSource;
class CSocketEvent final
{
public:
	enum EventType
	{
		hostaddress,

		// This is a nonfatal condition. It
		// means there are additional addresses to try.
		connection_next,
		connection,
		read,
		write,
		close
	};

	CSocketEvent(CSocketEventHandler* pSocketEventHandler, CSocketEventSource* pSource, EventType type, const wxChar* data);
	CSocketEvent(CSocketEventHandler* pSocketEventHandler, CSocketEventSource* pSource, EventType type, int error = 0);
	~CSocketEvent();

	CSocketEventSource* GetSocketEventSource() const { return m_pSource; }
	EventType GetType() const { return m_type; }
	CSocketEventHandler* GetSocketEventHandler() const { return m_pSocketEventHandler; }

	wxString GetData() const;
	int GetError() const { return m_error; }

protected:
	CSocketEventSource* m_pSource;
	const EventType m_type;
	wxChar *m_data;
	int m_error;
	CSocketEventHandler* m_pSocketEventHandler;

	friend class CSocketEventDispatcher;
};

class CSocketEventDispatcher final : public CEventHandler
{
public:
	CSocketEventDispatcher(CEventLoop & event_loop);
	~CSocketEventDispatcher();

	void SendEvent(CSocketEvent* evt);
	void RemovePending(const CSocketEventHandler* pHandler);
	void RemovePending(const CSocketEventSource* pSource);
	void UpdatePending(const CSocketEventHandler* pOldHandler, const CSocketEventSource* pOldSource, CSocketEventHandler* pNewHandler, CSocketEventSource* pNewSource);

private:
	virtual void operator()(CEventBase const& ev);

	std::list<CSocketEvent*> m_pending_events;

	wxCriticalSection m_sync;
};

class CSocketEventHandler
{
public:
	CSocketEventHandler(CSocketEventDispatcher& dispatcher)
		: dispatcher_(dispatcher)
	{};

	virtual ~CSocketEventHandler();

	virtual void OnSocketEvent(CSocketEvent& event) = 0;

	CSocketEventDispatcher& dispatcher_;
};

class CSocketEventSource
{
public:
	CSocketEventSource(CSocketEventDispatcher& dispatcher)
		: dispatcher_(dispatcher)
	{
	}

	virtual ~CSocketEventSource();

	CSocketEventDispatcher& dispatcher_;
};

class CCallback
{
public:
	virtual void cb() {}
};

class CSocketThread;
class CSocket : public CSocketEventSource
{
	friend class CSocketThread;
public:
	CSocket(CSocketEventHandler* pEvtHandler, CSocketEventDispatcher& dispatcher);
	virtual ~CSocket();

	CSocket(CSocket const&) = delete;
	CSocket& operator=(CSocket const&) = delete;

	enum SocketState
	{
		// How the socket is initially
		none,

		// Only in listening and connecting states you can get a connection event.
		// After sending the event, socket is in connected state
		listening,
		connecting,

		// Only in this state you can get send or receive events
		connected,

		// Graceful shutdown, you get close event once done
		closing,
		closed
	};
	SocketState GetState();

	enum address_family
	{
		unspec, // AF_UNSPEC
		ipv4,   // AF_INET
		ipv6    // AF_INET6
	};

	// Connects to the given host, given as name, IPv4 or IPv6 address.
	// Returns 0 on success, else an error code. Note: EINPROGRESS is
	// not really an error. On success, you should still wait for the
	// connection event.
	// If host is a name that can be resolved, a hostaddress socket event gets sent.
	// Once connections got established, a connection event gets sent. If
	// connection could not be established, a close event gets sent.
	int Connect(wxString host, unsigned int port, address_family family = unspec);

	// After receiving a send or receive event, you can call these functions
	// as long as their return value is positive.
	int Read(void *buffer, unsigned int size, int& error);
	int Peek(void *buffer, unsigned int size, int& error);
	int Write(const void *buffer, unsigned int size, int& error);

	int Close();

	// Returns empty string on error
	wxString GetLocalIP(bool strip_zone_index = false) const;
	wxString GetPeerIP(bool strip_zone_index = false) const;

	// Returns the hostname passed to Connect()
	wxString GetPeerHost() const;

	// -1 on error
	int GetLocalPort(int& error);
	int GetRemotePort(int& error);

	// If connected, either ipv4 or ipv6, unspec otherwise
	address_family GetAddressFamily() const;

	static wxString GetErrorString(int error);
	static wxString GetErrorDescription(int error);

	// Can only be called if the state is none
	void SetEventHandler(CSocketEventHandler* pEvtHandler);
	CSocketEventHandler* GetEventHandler() { return m_pEvtHandler; }

	static bool Cleanup(bool force);

	static wxString AddressToString(const struct sockaddr* addr, int addr_len, bool with_port = true, bool strip_zone_index = false);

	int Listen(address_family family, int port = 0);
	CSocket* Accept(int& error);

	enum Flags
	{
		flag_nodelay = 0x01,
		flag_keepalive = 0x02
	};

	int GetFlags() const { return m_flags; }
	void SetFlags(int flags);

	// If called on listen socket, sizes will be inherited by
	// accepted sockets
	void SetBufferSizes(int size_read, int size_write);

	void SetSynchronousReadCallback(CCallback* cb);

protected:
	static int DoSetFlags(int fd, int flags, int flags_mask);
	static int DoSetBufferSizes(int fd, int size_read, int size_write);
	static int SetNonblocking(int fd);

	void DetachThread();

	CSocketEventHandler* m_pEvtHandler;

	int m_fd;

	SocketState m_state;

	CSocketThread* m_pSocketThread;

	wxString m_host;
	unsigned int m_port;
	int m_family;

	int m_flags;

	int m_buffer_sizes[2];

	CCallback* m_synchronous_read_cb{};
};

#ifdef __WXMSW__

#ifndef EISCONN
#define EISCONN WSAEISCONN
#endif
#ifndef EINPROGRESS
#define EINPROGRESS WSAEINPROGRESS
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT WSAEAFNOSUPPORT
#endif
#ifndef EADDRINUSE
#define EADDRINUSE WSAEADDRINUSE
#endif
#ifndef ENOBUFS
#define ENOBUFS WSAENOBUFS
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT WSAEPROTONOSUPPORT
#endif
#ifndef EALREADY
#define EALREADY WSAEALREADY
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED WSAECONNREFUSED
#endif
#ifndef ENOTSOCK
#define ENOTSOCK WSAENOTSOCK
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT WSAETIMEDOUT
#endif
#ifndef ENETUNREACH
#define ENETUNREACH WSAENETUNREACH
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH WSAEHOSTUNREACH
#endif
#ifndef ENOTCONN
#define ENOTCONN WSAENOTCONN
#endif
#ifndef ENETRESET
#define ENETRESET WSAENETRESET
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP WSAEOPNOTSUPP
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN WSAESHUTDOWN
#endif
#ifndef EMSGSIZE
#define EMSGSIZE WSAEMSGSIZE
#endif
#ifndef ECONNABORTED
#define ECONNABORTED WSAECONNABORTED
#endif
#ifndef ECONNRESET
#define ECONNRESET WSAECONNRESET
#endif

// For the future:
// Handle ERROR_NETNAME_DELETED=64
#endif //__WXMSW__

#endif //__SOCKET_H__
