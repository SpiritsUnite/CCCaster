#include "Event.h"
#include "Test.h"
#include "Log.h"
#include "Util.h"
#include "Socket.h"
#include "Timer.h"
#include "GoBackN.h"

#include <gtest/gtest.h>

#include <vector>

#define TEST_PORT       258258

using namespace std;

int RunAllTests ( int& argc, char *argv[] )
{
    testing::InitGoogleTest ( &argc, argv );
    int result = RUN_ALL_TESTS();

    // Final timeout test with EventManager::release();
    {
        struct TestSocket : public Socket::Owner, public Timer::Owner
        {
            shared_ptr<Socket> socket;
            Timer timer;

            void timerExpired ( Timer *timer ) { EventManager::get().release(); }

            TestSocket ( const string& address, unsigned port )
                : socket ( Socket::connect ( this, address, port, Protocol::TCP ) ), timer ( this )
            {
                timer.start ( 1000 );
            }
        };

        TestSocket client ( "google.com" , 23456 );

        EventManager::get().start();

        assert ( client.socket->isConnected() == false );
    }

    return result;
}

TEST ( Socket, TcpConnect )
{
    struct TestSocket : public Socket::Owner, public Timer::Owner
    {
        shared_ptr<Socket> socket, accepted;
        Timer timer;

        void acceptEvent ( Socket *serverSocket ) { accepted = serverSocket->accept ( this ); }

        void timerExpired ( Timer *timer ) { EventManager::get().stop(); }

        TestSocket ( unsigned port )
            : socket ( Socket::listen ( this, port, Protocol::TCP ) ), timer ( this )
        {
            timer.start ( 1000 );
        }

        TestSocket ( const string& address, unsigned port )
            : socket ( Socket::connect ( this, address, port, Protocol::TCP ) ), timer ( this )
        {
            timer.start ( 1000 );
        }
    };

    TestSocket server ( TEST_PORT );
    TestSocket client ( "127.0.0.1", TEST_PORT );

    EventManager::get().start();

    EXPECT_TRUE ( server.socket->isConnected() );
    EXPECT_TRUE ( server.accepted->isConnected() );
    EXPECT_TRUE ( client.socket->isConnected() );
}

TEST ( Socket, TcpTimeout )
{
    struct TestSocket : public Socket::Owner, public Timer::Owner
    {
        shared_ptr<Socket> socket;
        Timer timer;

        void timerExpired ( Timer *timer ) { EventManager::get().stop(); }

        TestSocket ( const string& address, unsigned port )
            : socket ( Socket::connect ( this, address, port, Protocol::TCP ) ), timer ( this )
        {
            timer.start ( 1000 );
        }
    };

    TestSocket client ( "127.0.0.1", TEST_PORT );

    EventManager::get().start();

    EXPECT_FALSE ( client.socket->isConnected() );
}

TEST ( Socket, TcpSend )
{
    struct TestSocket : public Socket::Owner, public Timer::Owner
    {
        shared_ptr<Socket> socket, accepted;
        Timer timer;
        MsgPtr msg;

        void acceptEvent ( Socket *serverSocket )
        {
            accepted = serverSocket->accept ( this );
            accepted->send ( TestMessage ( "Hello client!" ) );
        }

        void connectEvent ( Socket *socket )
        {
            socket->send ( TestMessage ( "Hello server!" ) );
        }

        void readEvent ( Socket *socket, char *bytes, size_t len, const IpAddrPort& address )
        {
            msg = Serializable::decode ( bytes, len );
        }

        void timerExpired ( Timer *timer ) { EventManager::get().stop(); }

        TestSocket ( unsigned port )
            : socket ( Socket::listen ( this, port, Protocol::TCP ) ), timer ( this )
        {
            timer.start ( 1000 );
        }

        TestSocket ( const string& address, unsigned port )
            : socket ( Socket::connect ( this, address, port, Protocol::TCP ) ), timer ( this )
        {
            timer.start ( 1000 );
        }
    };

    TestSocket server ( TEST_PORT );
    TestSocket client ( "127.0.0.1", TEST_PORT );

    EventManager::get().start();

    EXPECT_TRUE ( server.socket->isConnected() );
    EXPECT_TRUE ( server.msg.get() );

    if ( server.msg.get() )
    {
        EXPECT_EQ ( server.msg->type(), MsgType::TestMessage );
        EXPECT_EQ ( server.msg->getAs<TestMessage>().str, "Hello server!" );
    }

    EXPECT_TRUE ( client.socket->isConnected() );
    EXPECT_TRUE ( client.msg.get() );

    if ( client.msg.get() )
    {
        EXPECT_EQ ( client.msg->type(), MsgType::TestMessage );
        EXPECT_EQ ( client.msg->getAs<TestMessage>().str, "Hello client!" );
    }
}

TEST ( Socket, UdpSend )
{
    struct TestSocket : public Socket::Owner, public Timer::Owner
    {
        shared_ptr<Socket> socket;
        Timer timer;
        MsgPtr msg;
        bool sent;

        void readEvent ( Socket *socket, char *bytes, size_t len, const IpAddrPort& address )
        {
            msg = Serializable::decode ( bytes, len );

            if ( socket->getRemoteAddress().addr.empty() )
            {
                socket->send ( TestMessage ( "Hello client!" ), address );
                sent = true;
            }
        }

        void timerExpired ( Timer *timer )
        {
            if ( !sent )
            {
                if ( !socket->getRemoteAddress().addr.empty() )
                {
                    socket->send ( TestMessage ( "Hello server!" ) );
                    sent = true;
                }

                timer->start ( 1000 );
            }
            else
            {
                EventManager::get().stop();
            }
        }

        TestSocket ( unsigned port )
            : socket ( Socket::listen ( this, port, Protocol::UDP ) ), timer ( this ), sent ( false )
        {
            timer.start ( 1000 );
        }

        TestSocket ( const string& address, unsigned port )
            : socket ( Socket::connect ( this, address, port, Protocol::UDP ) ), timer ( this ), sent ( false )
        {
            timer.start ( 1000 );
        }
    };

    TestSocket server ( TEST_PORT );
    TestSocket client ( "127.0.0.1", TEST_PORT );

    EventManager::get().start();

    EXPECT_TRUE ( server.socket->isConnected() );
    EXPECT_TRUE ( server.msg.get() );

    if ( server.msg.get() )
    {
        EXPECT_EQ ( server.msg->type(), MsgType::TestMessage );
        EXPECT_EQ ( server.msg->getAs<TestMessage>().str, "Hello server!" );
    }

    EXPECT_TRUE ( client.socket->isConnected() );
    EXPECT_TRUE ( client.msg.get() );

    if ( client.msg.get() )
    {
        EXPECT_EQ ( client.msg->type(), MsgType::TestMessage );
        EXPECT_EQ ( client.msg->getAs<TestMessage>().str, "Hello client!" );
    }
}

TEST ( GoBackN, SendOnce )
{
    struct TestSocket : public GoBackN::Owner, public Socket::Owner, public Timer::Owner
    {
        shared_ptr<Socket> socket;
        IpAddrPort address;
        GoBackN gbn;
        Timer timer;
        MsgPtr msg;
        bool server;

        void send ( const MsgPtr& msg )
        {
            socket->send ( *msg, address );
        }

        void recv ( const MsgPtr& msg )
        {
            this->msg = msg;
            LOG ( "Stopping because msg has been received" );
            EventManager::get().stop();
        }

        void readEvent ( Socket *socket, char *bytes, size_t len, const IpAddrPort& address )
        {
            if ( this->address.empty() )
                this->address = address;

            msg = Serializable::decode ( bytes, len );

            if ( rand() % 100 < 10 )
                gbn.recv ( msg );
        }

        void timerExpired ( Timer *timer )
        {
            if ( socket->isClient() )
            {
                gbn.send ( MsgPtr ( new TestMessage ( "Hello server!" ) ) );
            }
            else
            {
                LOG ( "Stopping because of timeout" );
                EventManager::get().stop();
            }
        }

        TestSocket ( unsigned port )
            : socket ( Socket::listen ( this, port, Protocol::UDP ) ), gbn ( this ), timer ( this )
        {
            timer.start ( 1000 * 10 );
        }

        TestSocket ( const string& address, unsigned port )
            : socket ( Socket::connect ( this, address, port, Protocol::UDP ) )
            , address ( address, port ), gbn ( this ), timer ( this )
        {
            timer.start ( 1000 );
        }
    };

    TestSocket server ( TEST_PORT );
    TestSocket client ( "127.0.0.1", TEST_PORT );

    EventManager::get().start();

    EXPECT_TRUE ( server.msg.get() );

    if ( server.msg.get() )
    {
        EXPECT_EQ ( server.msg->type(), MsgType::TestMessage );
        EXPECT_EQ ( server.msg->getAs<TestMessage>().str, "Hello server!" );
    }
}

TEST ( GoBackN, SendSequential )
{
    struct TestSocket : public GoBackN::Owner, public Socket::Owner, public Timer::Owner
    {
        shared_ptr<Socket> socket;
        IpAddrPort address;
        GoBackN gbn;
        Timer timer;
        vector<MsgPtr> msgs;

        void send ( const MsgPtr& msg )
        {
            socket->send ( *msg, address );
        }

        void recv ( const MsgPtr& msg )
        {
            msgs.push_back ( msg );

            if ( msgs.size() == 5 )
            {
                LOG ( "Stopping because all msgs have been received" );
                EventManager::get().stop();
            }
        }

        void readEvent ( Socket *socket, char *bytes, size_t len, const IpAddrPort& address )
        {
            if ( this->address.empty() )
                this->address = address;

            MsgPtr msg = Serializable::decode ( bytes, len );

            if ( rand() % 100 < 50 )
                gbn.recv ( msg );
        }

        void timerExpired ( Timer *timer )
        {
            if ( socket->isClient() )
            {
                gbn.send ( MsgPtr ( new TestMessage ( "Message 1" ) ) );
                gbn.send ( MsgPtr ( new TestMessage ( "Message 2" ) ) );
                gbn.send ( MsgPtr ( new TestMessage ( "Message 3" ) ) );
                gbn.send ( MsgPtr ( new TestMessage ( "Message 4" ) ) );
                gbn.send ( MsgPtr ( new TestMessage ( "Message 5" ) ) );
            }
            else
            {
                LOG ( "Stopping because of timeout" );
                EventManager::get().stop();
            }
        }

        TestSocket ( unsigned port )
            : socket ( Socket::listen ( this, port, Protocol::UDP ) ), gbn ( this ), timer ( this )
        {
            timer.start ( 1000 * 30 );
        }

        TestSocket ( const string& address, unsigned port )
            : socket ( Socket::connect ( this, address, port, Protocol::UDP ) )
            , address ( address, port ), gbn ( this ), timer ( this )
        {
            timer.start ( 1000 );
        }
    };

    TestSocket server ( TEST_PORT );
    TestSocket client ( "127.0.0.1", TEST_PORT );

    EventManager::get().start();

    EXPECT_EQ ( server.msgs.size(), 5 );

    for ( size_t i = 0; i < server.msgs.size(); ++i )
    {
        LOG ( "Server got '%s'", server.msgs[i]->getAs<TestMessage>().str.c_str() );
        EXPECT_EQ ( server.msgs[i]->type(), MsgType::TestMessage );
        EXPECT_EQ ( server.msgs[i]->getAs<TestMessage>().str, toString ( "Message %u", i + 1 ) );
    }
}

TEST ( GoBackN, SendAndRecv )
{
    struct TestSocket : public GoBackN::Owner, public Socket::Owner, public Timer::Owner
    {
        shared_ptr<Socket> socket;
        IpAddrPort address;
        GoBackN gbn;
        Timer timer;
        vector<MsgPtr> msgs;
        bool sent;

        void send ( const MsgPtr& msg )
        {
            if ( !address.empty() )
                socket->send ( *msg, address );
        }

        void recv ( const MsgPtr& msg )
        {
            msgs.push_back ( msg );

            if ( msgs.size() == 5 && gbn.getAckCount() == 5 )
            {
                LOG ( "Stopping because all msgs have been received" );
                EventManager::get().stop();
            }
        }

        void readEvent ( Socket *socket, char *bytes, size_t len, const IpAddrPort& address )
        {
            if ( this->address.empty() )
                this->address = address;

            MsgPtr msg = Serializable::decode ( bytes, len );

            if ( rand() % 100 < 50 )
                gbn.recv ( msg );
        }

        void timerExpired ( Timer *timer )
        {
            if ( !sent )
            {
                gbn.send ( MsgPtr ( new TestMessage ( socket->isClient() ? "Client 1" : "Server 1" ) ) );
                gbn.send ( MsgPtr ( new TestMessage ( socket->isClient() ? "Client 2" : "Server 2" ) ) );
                gbn.send ( MsgPtr ( new TestMessage ( socket->isClient() ? "Client 3" : "Server 3" ) ) );
                gbn.send ( MsgPtr ( new TestMessage ( socket->isClient() ? "Client 4" : "Server 4" ) ) );
                gbn.send ( MsgPtr ( new TestMessage ( socket->isClient() ? "Client 5" : "Server 5" ) ) );
                sent = true;
                timer->start ( 1000 * 120 );
            }
            else
            {
                LOG ( "Stopping because of timeout" );
                EventManager::get().stop();
            }
        }

        TestSocket ( unsigned port )
            : socket ( Socket::listen ( this, port, Protocol::UDP ) ), gbn ( this ), timer ( this ), sent ( false )
        {
            timer.start ( 1000 );
        }

        TestSocket ( const string& address, unsigned port )
            : socket ( Socket::connect ( this, address, port, Protocol::UDP ) )
            , address ( address, port ), gbn ( this ), timer ( this ), sent ( false )
        {
            timer.start ( 1000 );
        }
    };

    TestSocket server ( TEST_PORT );
    TestSocket client ( "127.0.0.1", TEST_PORT );

    EventManager::get().start();

    EXPECT_EQ ( server.msgs.size(), 5 );

    for ( size_t i = 0; i < server.msgs.size(); ++i )
    {
        LOG ( "Server got '%s'", server.msgs[i]->getAs<TestMessage>().str.c_str() );
        EXPECT_EQ ( server.msgs[i]->type(), MsgType::TestMessage );
        EXPECT_EQ ( server.msgs[i]->getAs<TestMessage>().str, toString ( "Client %u", i + 1 ) );
    }

    EXPECT_EQ ( client.msgs.size(), 5 );

    for ( size_t i = 0; i < client.msgs.size(); ++i )
    {
        LOG ( "Client got '%s'", client.msgs[i]->getAs<TestMessage>().str.c_str() );
        EXPECT_EQ ( client.msgs[i]->type(), MsgType::TestMessage );
        EXPECT_EQ ( client.msgs[i]->getAs<TestMessage>().str, toString ( "Server %u", i + 1 ) );
    }
}