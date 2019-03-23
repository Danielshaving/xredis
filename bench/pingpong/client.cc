#pragma once

#include "all.h"
#include "tcpclient.h"
#include "tcpconnection.h"
#include "threadpool.h"
#include "log.h"

class Client;

class Connect {
public:
    Connect(EventLoop *loop,
            const char *ip, uint16_t port, Client *owner)
            : cli(loop, ip, port, nullptr),
              ip(ip),
              port(port),
              owner(owner),
              bytesRead(0),
              bytesWritten(0),
              messagesRead(0) {
        cli.setConnectionCallback(std::bind(&Connect::connCallBack, this, std::placeholders::_1));
        cli.setMessageCallback(std::bind(&Connect::readCallBack, this, std::placeholders::_1, std::placeholders::_2));
    }

    void start() {
        cli.connect();
    }

    void stop() {
        cli.disConnect();
    }

    int64_t getBytesRead() { return bytesRead; }

    int64_t getMessagesRead() { return messagesRead; }

private:
    void connCallBack(const TcpConnectionPtr &conn);

    void readCallBack(const TcpConnectionPtr &conn, Buffer *buf) {
        ++messagesRead;
        bytesRead += buf->readableBytes();
        bytesWritten += buf->readableBytes();
        conn->send(buf);
        buf->retrieveAll();
    }

    TcpClient cli;
    const char *ip;
    uint16_t port;
    Client *owner;
    int64_t bytesRead;
    int64_t bytesWritten;
    int64_t messagesRead;

};

class Client {
public:
    Client(EventLoop *loop, const char *ip, uint16_t port, int blockSize, int sessionCount,
           int timeOut, int threadCount)
            : loop(loop),
              threadPool(loop),
              sessionCount(sessionCount),
              timeOut(timeOut) {
        loop->runAfter(timeOut, false, std::bind(&Client::handlerTimeout, this));
        if (threadCount > 1) {
            threadPool.setThreadNum(threadCount);
        }

        threadPool.start();

        for (int i = 0; i < blockSize; i++) {
            message.push_back(static_cast<char>(i % 128));
        }

        for (int i = 0; i < sessionCount; i++) {
            std::shared_ptr <Connect> vsession(new Connect(threadPool.getNextLoop(), ip, port, this));
            vsession->start();
            sessions.push_back(vsession);
        }
    }

    void onConnect() {
        if (++numConencted == sessionCount) {
            LOG_WARN << "all connected";
        }
    }

    void onDisconnect(const TcpConnectionPtr &conn) {
        numConencted--;
        if (numConencted == 0) {
            LOG_WARN << "all disconnected";

            int64_t totalBytesRead = 0;
            int64_t totalMessagesRead = 0;
            for (auto it = sessions.begin(); it != sessions.end(); ++it) {
                totalBytesRead += (*it)->getBytesRead();
                totalMessagesRead += (*it)->getMessagesRead();
            }

            LOG_WARN << totalBytesRead << " total bytes read";
            LOG_WARN << totalMessagesRead << " total messages read";
            LOG_WARN << static_cast<double>(totalBytesRead) / static_cast<double>(totalMessagesRead)
                     << " average message size";
            LOG_WARN << static_cast<double>(totalBytesRead) / (timeOut * 1024 * 1024) << " MiB/s throughput";
            conn->getLoop()->queueInLoop(std::bind(&Client::quit, this));
        }
    }

    const std::string &getMessage() const { return message; }

    void quit() {
        loop->queueInLoop(std::bind(&EventLoop::quit, loop));
    }

    void handlerTimeout() {
        LOG_WARN << "stop";
        std::for_each(sessions.begin(), sessions.end(), std::mem_fn(&Connect::stop));
    }

private:
    EventLoop *loop;
    ThreadPool threadPool;
    int sessionCount;
    int timeOut;
    std::vector <std::shared_ptr<Connect>> sessions;
    std::string message;
    std::atomic<int> numConencted;
};

void Connect::connCallBack(const TcpConnectionPtr &conn) {
    if (conn->connected()) {
        conn->send(owner->getMessage());
        owner->onConnect();
    } else {
        owner->onDisconnect(conn);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 7) {
        fprintf(stderr, "Usage: Client <host_ip> <port> <threads> <blocksize> ");
        fprintf(stderr, "<sessions> <time>\n");
    } else {
        LOG_INFO << "ping pong Client pid = " << getpid() << ", tid = " << getpid();
        const char *ip = argv[1];
        uint16_t port = static_cast<uint16_t>(atoi(argv[2]));
        int threadCount = atoi(argv[3]);
        int blockSize = atoi(argv[4]);
        int sessionCount = atoi(argv[5]);
        int timeout = atoi(argv[6]);

        EventLoop loop;
        Client cli(&loop, ip, port, blockSize, sessionCount, timeout, threadCount);
        loop.run();

    }
    return 0;
}

