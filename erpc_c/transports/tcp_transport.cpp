/*
 * Copyright (c) 2015, Freescale Semiconductor, Inc.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * o Redistributions of source code must retain the above copyright notice, this list
 *   of conditions and the following disclaimer.
 *
 * o Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * o Neither the name of Freescale Semiconductor, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "tcp_transport.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <errno.h>
#include <err.h>
#include <new>

#if !(__embedded_cplusplus)
using namespace std;
#endif

using namespace erpc;

// Set this to 1 to enabke debug logging.
// TODO fix issue with the transport not working on Linux if debug logging is disabled.
#define TCP_TRANSPORT_DEBUG_LOG (1)

#if TCP_TRANSPORT_DEBUG_LOG
#define TCP_DEBUG_PRINT(_fmt_, ...) printf(_fmt_, ##__VA_ARGS__)
#define TCP_DEBUG_ERR(_msg_) err(errno, _msg_)
#else
#define TCP_DEBUG_PRINT(_fmt_, ...)
#define TCP_DEBUG_ERR(_msg_)
#endif

////////////////////////////////////////////////////////////////////////////////
// Code
////////////////////////////////////////////////////////////////////////////////

TCPTransport::TCPTransport(bool isServer)
: m_isServer(isServer)
, m_host(NULL)
, m_port(0)
, m_socket(-1)
, m_serverThread(serverThreadStub)
, m_runServer(true)
{
}

TCPTransport::TCPTransport(const char *host, uint16_t port, bool isServer)
: m_isServer(isServer)
, m_host(host)
, m_port(port)
, m_socket(-1)
, m_serverThread(serverThreadStub)
, m_runServer(true)
{
}

TCPTransport::~TCPTransport()
{
}

void TCPTransport::configure(const char *host, uint16_t port)
{
    m_host = host;
    m_port = port;
}

erpc_status_t TCPTransport::open()
{
    if (m_isServer)
    {
        m_runServer = true;
        m_serverThread.start(this);
        return kErpcStatus_Success;
    }
    else
    {
        return connectClient();
    }
}

erpc_status_t TCPTransport::connectClient()
{
    if (m_socket != -1)
    {
        TCP_DEBUG_PRINT("socket already connected\n");
        return kErpcStatus_Success;
    }

    // Fill in hints structure for getaddrinfo.
    struct addrinfo hints = { 0 };
    hints.ai_flags = AI_NUMERICSERV;
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    // Convert port number to a string.
    char portString[8];
    snprintf(portString, sizeof(portString), "%d", m_port);

    // Perform the name lookup.
    struct addrinfo *res0;
    int result = getaddrinfo(m_host, portString, &hints, &res0);
    if (result)
    {
        // TODO check EAI_NONAME
        TCP_DEBUG_ERR("gettaddrinfo failed");
        return kErpcStatus_UnknownName;
    }

    // Iterate over result addresses and try to connect. Exit the loop on the first successful
    // connection.
    int sock = -1;
    struct addrinfo *res;
    for (res = res0; res; res = res->ai_next)
    {
        // Create the socket.
        sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0)
        {
            continue;
        }

        // Attempt to connect.
        if (connect(sock, res->ai_addr, res->ai_addrlen) < 0)
        {
            ::close(sock);
            sock = -1;
            continue;
        }

        // Exit the loop for the first successful connection.
        break;
    }

    // Free the result list.
    freeaddrinfo(res0);

    // Check if we were able to open a connection.
    if (sock < 0)
    {
        // TODO check EADDRNOTAVAIL:
        TCP_DEBUG_ERR("connecting failed");
        return kErpcStatus_ConnectionFailure;
    }

// On some systems (BSD) we can disable SIGPIPE on the socket. For others (Linux), we have to
// ignore SIGPIPE.
#if defined(SO_NOSIGPIPE)
    // Disable SIGPIPE for this socket. This will cause write() to return an EPIPE error if the
    // other side has disappeared instead of our process receiving a SIGPIPE.
    int set = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int)) < 0)
    {
        ::close(sock);
        TCP_DEBUG_ERR("setsockopt failed");
        return kErpcStatus_Fail;
    }
#else
    // globally disable the SIGPIPE signal
    signal(SIGPIPE, SIG_IGN);
#endif // defined(SO_NOSIGPIPE)
    m_socket = sock;

    return kErpcStatus_Success;
}

erpc_status_t TCPTransport::close()
{
    if (m_isServer)
    {
        m_runServer = false;
    }

    if (m_socket != -1)
    {
        ::close(m_socket);
        m_socket = -1;
    }

    return kErpcStatus_Success;
}

erpc_status_t TCPTransport::underlyingReceive(uint8_t *data, uint32_t size)
{
    // Block until we have a valid connection.
    while (m_socket <= 0)
    {
        // Sleep 10 ms.
        Thread::sleep(10000);
    }

    ssize_t length = 0;

    // Loop until all requested data is received.
    while (size)
    {
        length = read(m_socket, data, size);

        // Length will be zero if the connection is closed.
        if (length == 0)
        {
            close();
            return kErpcStatus_ConnectionClosed;
        }
        else if (length < 0)
        {
            return kErpcStatus_ReceiveFailed;
        }
        else
        {
            size -= length;
            data += length;
        }
    }

    return length < 0 ? kErpcStatus_ReceiveFailed : kErpcStatus_Success;
}

erpc_status_t TCPTransport::underlyingSend(const uint8_t *data, uint32_t size)
{
    if (m_socket <= 0)
    {
        return kErpcStatus_Success;
    }

    // Loop until all data is sent.
    while (size)
    {
        ssize_t result = write(m_socket, data, size);
        if (result >= 0)
        {
            size -= result;
            data += result;
        }
        else
        {
            if (errno == EPIPE)
            {
                // Server closed.
                close();
                return kErpcStatus_ConnectionClosed;
            }
            return kErpcStatus_SendFailed;
        }
    }

    return kErpcStatus_Success;
}

void TCPTransport::serverThread()
{
    TCP_DEBUG_PRINT("in server thread\n");

    // Create socket.
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0)
    {
        TCP_DEBUG_ERR("failed to create server socket");
        return;
    }

    // Fill in address struct.
    struct sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY; // htonl(local ? INADDR_LOOPBACK : INADDR_ANY);
    serverAddress.sin_port = htons(m_port);

    // Turn on reuse address option.
    int yes = 1;
    int result = setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (result < 0)
    {
        TCP_DEBUG_ERR("setsockopt failed");
        ::close(serverSocket);
        return;
    }

    // Bind socket to address.
    result = bind(serverSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress));
    if (result < 0)
    {
        TCP_DEBUG_ERR("bind failed");
        ::close(serverSocket);
        return;
    }

    // Listen for connections.
    result = listen(serverSocket, 1);
    if (result < 0)
    {
        TCP_DEBUG_ERR("listen failed");
        ::close(serverSocket);
        return;
    }

    TCP_DEBUG_PRINT("Listening for connections\n");

    while (m_runServer)
    {
        struct sockaddr incomingAddress;
        socklen_t incomingAddressLength;
        int incomingSocket = accept(serverSocket, &incomingAddress, &incomingAddressLength);
        if (incomingSocket > 0)
        {
            // Successfully accepted a connection.
            m_socket = incomingSocket;
        }
        else
        {
            TCP_DEBUG_ERR("accept failed");
        }
    }

    ::close(serverSocket);
}

void TCPTransport::serverThreadStub(void *arg)
{
    TCPTransport *This = reinterpret_cast<TCPTransport *>(arg);
    TCP_DEBUG_PRINT("in serverThreadStub (arg=%p)\n", arg);
    if (This)
    {
        This->serverThread();
    }
}
