#pragma once
#include "all.h"
#include "hiredis.h"
#include "tcpserver.h"
#include "proxysession.h"

class RedisProxy
{
public:
	RedisProxy(const char *ip, int16_t port, const char *redisIp,
		int16_t redisPort, int16_t threadCount, int16_t sessionCount);
	~RedisProxy();

	void proxyConnCallback(const TcpConnectionPtr &conn);
	void processCommand(const TcpConnectionPtr &conn, const char *buf, size_t len);
	void reset();
	void run();
	
	void initRedisPorxy();
	void initRedisAsync();
	void initRedisSync();
	void initRedisCommand();
	void initRedisTimer();
	
	bool getRedisCommand(const RedisObjectPtr &command);
	bool handleRedisCommand(const RedisObjectPtr &command, 
		const ProxySessionPtr &session, const std::deque<RedisObjectPtr> &objs);
	bool hgetallCommand(const std::deque<RedisObjectPtr> &obj, const ProxySessionPtr &session);
	
	void redisContextTimer();
	void highWaterCallBack(const TcpConnectionPtr &conn, size_t bytesToSent);
	void writeCompleteCallBack(const TcpConnectionPtr &conn);

	void redisConnCallback(const TcpConnectionPtr &conn);
	void redisDisconnCallback(const TcpConnectionPtr &conn);

	void proxyCallback(const RedisAsyncContextPtr &c,
		const RedisReplyPtr &reply, const std::any &privdata);
private:
	EventLoop loop;
	TcpServer server;
	std::mutex mutex;
	const char *ip;
	int16_t port;
	const char *redisIp;
	int16_t redisPort;
	int16_t threadCount;
	int16_t sessionCount;
	
	static const int32_t kHeart = 10;
	static const int32_t kHighWaterBytes = 1024 * 1024 * 64;
	
	std::unordered_map<int32_t, ProxySessionPtr> sessions;
	std::unordered_map<std::thread::id, std::shared_ptr<Hiredis>> threadHiredis;
	std::unordered_map<std::thread::id, std::vector<RedisContextPtr>> threadRedisContexts;
	typedef std::function<bool(const std::deque<RedisObjectPtr> &, const ProxySessionPtr &)> CommandFunc;
	std::unordered_map<RedisObjectPtr, CommandFunc, Hash, Equal> redisCommands;
};