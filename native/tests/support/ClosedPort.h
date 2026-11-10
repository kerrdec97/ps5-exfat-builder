#pragma once

// A loopback TCP port that refuses connections for as long as the object lives.
//
// The port is bound but never listens, so a connect is refused and the system
// cannot hand the number to another program meanwhile (a port that was only
// listened on and closed again can be taken by a test running in parallel).
// Header-only: only tests that already link Qt Network include it.

#include <QHostAddress>
#include <QTcpSocket>

namespace fwtest {

class ClosedPort {
public:
    ClosedPort() { m_bound = m_socket.bind(QHostAddress::LocalHost, 0, QAbstractSocket::DontShareAddress); }
    ClosedPort(const ClosedPort&) = delete;
    ClosedPort& operator=(const ClosedPort&) = delete;

    // False when no port could be reserved; port() is then 0.
    bool isValid() const { return m_bound && m_socket.localPort() != 0; }
    quint16 port() const { return m_socket.localPort(); }

private:
    QTcpSocket m_socket;
    bool m_bound = false;
};

// One such port for the whole run of a test program, for the many places that
// only need "nothing answers here". Never released: the system takes it back
// when the program ends. Call it on the test's main thread.
inline quint16 refusedPort()
{
    static const ClosedPort* const reserved = new ClosedPort;
    return reserved->port();
}

} // namespace fwtest
