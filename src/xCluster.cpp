#include "xCluster.h"
#include "xRedis.h"
#include "xLog.h"
#include "xCrc16.h"

xCluster::xCluster():state(true),
isConnect(false)
{

}


xCluster::~xCluster()
{

}

void xCluster::readCallBack(const xTcpconnectionPtr& conn, xBuffer* recvBuf, void *data)
{
	while (recvBuf->readableBytes() > 0)
	{
		if (memcmp(recvBuf->peek(), shared.ok->ptr, 5) == 0)
		{	
			/*
			auto iter = clusterSlotNodes.find(*it);
			if(iter !=  clusterSlotNodes.end())
			{
				iter->second.ip = ip;
				iter->second.port = port;
			}
			*/
			
			recvBuf->retrieve(5);
			std::unique_lock <std::mutex> lck(redis->clusterMutex);	
			if(redis->clusterMigratCached.readableBytes()> 0 )
			{
				if(conn->connected())
				{
					conn->send(&redis->clusterMigratCached);
				}

				xBuffer buff;
				buff.swap(redis->clusterMigratCached);
			}

			migratingSlosTos.clear();
			LOG_INFO<<"cluster migrate success " <<conn->host<<" " <<conn->port;
		
		}
		else
		{
			conn->forceClose();
			recvBuf->retrieveAll();
			LOG_INFO<<"cluster migrate failure " <<conn->host<<" " <<conn->port;
			break;
		}

	}
}


void xCluster::clusterRedirectClient(xSession * session, xClusterNode * n, int hashSlot, int errCode)
{
	if (errCode == CLUSTER_REDIR_CROSS_SLOT) {
		addReplySds(session->sendBuf, sdsnew("-CROSSSLOT Keys in request don't hash to the same slot\r\n"));
	}
	else if (errCode == CLUSTER_REDIR_UNSTABLE) {
		/* The request spawns mutliple keys in the same slot,
		* but the slot is not "stable" currently as there is
		* a migration or import in progress. */
		addReplySds(session->sendBuf,  sdsnew("-TRYAGAIN Multiple keys request during rehashing of slot\r\n"));
	}
	else if (errCode == CLUSTER_REDIR_DOWN_STATE) {
		addReplySds(session->sendBuf,  sdsnew("-CLUSTERDOWN The cluster is down\r\n"));
	}
	else if (errCode == CLUSTER_REDIR_DOWN_UNBOUND) {
		addReplySds(session->sendBuf, sdsnew("-CLUSTERDOWN Hash slot not served\r\n"));
	}
	else if (errCode == CLUSTER_REDIR_MOVED ||
		errCode == CLUSTER_REDIR_ASK)
	{
		addReplySds(session->sendBuf,  sdscatprintf(sdsempty(),
			"-%s %d %s:%d\r\n",
			(errCode == CLUSTER_REDIR_ASK) ? "ASK" : "MOVED",
			hashSlot, n->ip.c_str(), n->port));
	}
	else {
		LOG_WARN << "getNodeByQuery() unknown error.";
	}
} 


void xCluster::syncClusterSlot(std::deque<rObj*> &robj)
{
	{
		std::unique_lock <std::mutex> lck(redis->clusterMutex);
		for (auto it = redis->clustertcpconnMaps.begin(); it != redis->clustertcpconnMaps.end(); it++)
		{
			xBuffer sendBuf;
			redis->structureRedisProtocol(sendBuf, robj);
			it->second->send(&sendBuf);
		}
	}
	for (auto it = robj.begin(); it != robj.end(); it++)
	{
		zfree(*it);
	}

}

unsigned int xCluster::keyHashSlot(char *key, int keylen)
{
	int s, e; /* start-end indexes of { and } */

	for (s = 0; s < keylen; s++)
		if (key[s] == '{') break;

	/* No '{' ? Hash the whole key. This is the base case. */
	if (s == keylen) return crc16(key, keylen) & 0x3FFF;

	/* '{' found? Check if we have the corresponding '}'. */
	for (e = s + 1; e < keylen; e++)
		if (key[e] == '}') break;

	/* No '}' or nothing betweeen {} ? Hash the whole key. */
	if (e == keylen || e == s + 1) return crc16(key, keylen) & 0x3FFF;

	/* If we are here there is both a { and a } on its right. Hash
	* what is in the middle between { and }. */
	return crc16(key + s + 1, e - s - 1) & 0x3FFF;
}

int xCluster::getSlotOrReply(xSession  * session,rObj * o)
{
	long long slot;

	if (getLongLongFromObject(o, &slot) != REDIS_OK ||
		slot < 0 || slot >= CLUSTER_SLOTS)
	{
		addReplyError(session->sendBuf, "Invalid or out of range slot");
		return  REDIS_ERR;
	}
	return (int)slot;
}

void xCluster::structureProtocolSetCluster(std::string host, int32_t port, xBuffer &sendBuf, std::deque<rObj*> &robj,const xTcpconnectionPtr & conn)
{
	rObj * ip = createStringObject((char*)(host.c_str()), host.length());
	char buf[32];
	int32_t len = ll2string(buf, sizeof(buf), port);
	rObj * p = createStringObject(buf, len);
	robj.push_back(ip);
	robj.push_back(p);
	redis->structureRedisProtocol(sendBuf, robj);
	robj.pop_back();
	robj.pop_back();
	zfree(ip);
	zfree(p);
	conn->send(&sendBuf);
	sendBuf.retrieveAll();
}


void xCluster::delClusterImport(std::deque<rObj*> &robj)
{
	for (auto it = redis->clustertcpconnMaps.begin(); it != redis->clustertcpconnMaps.end(); it++)
	{
		xBuffer sendBuf;
		redis->structureRedisProtocol(sendBuf, robj);
		it->second->send(&sendBuf);
	}

	for (auto it = robj.begin(); it != robj.end(); it++)
	{
		zfree(*it);
	}
}


bool  xCluster::asyncReplicationToNode(std::string ip,int32_t port)
{
	std::unordered_set<int32_t>  uset;
	xTcpconnectionPtr conn;
	std::string ipPort = ip + "::" + std::to_string(port);
	{
		std::unique_lock <std::mutex> lck(redis->clusterMutex);
		auto it = migratingSlosTos.find(ipPort);
		if(it == migratingSlosTos.end() ||it->second.size() == 0 )
		{
			LOG_INFO<<"migratingSlosTos not  found " << ipPort;
			return false;
		}
			
		 uset = it->second;

		 for(auto iter = redis->clustertcpconnMaps.begin(); iter != redis->clustertcpconnMaps.end(); iter++)
		 {
		 	if(iter->second->host == ip && iter->second->port == port)
		 	{
		 		conn = iter->second;
				break;
		 	}
		 }
	}

	
	xBuffer sendBuf;
	redis->clusterRepliMigratEnabled = true;
	std::deque<rObj*> deques;
	for(auto it = redis->setMapShards.begin(); it != redis->setMapShards.end();it++)
	{
		{
			rObj * o = createStringObject("set", 3);
			std::unique_lock <std::mutex> lck((*it).mtx);
			for(auto iter = (*it).setMap.begin(); iter !=  (*it).setMap.end(); iter ++)
			{
				unsigned int slot = keyHashSlot((char*)iter->first->ptr,sdslen(iter->first->ptr));
				auto iterr = uset.find(slot);
				if(iterr != uset.end())
				{
					deques.push_back(o);
					deques.push_back(iter->first);
					deques.push_back(iter->second);
					redis->structureRedisProtocol(sendBuf,deques);
					if(conn->connected())
					{
						conn->sendPipe(&sendBuf);
					}
					else
					{
						LOG_INFO<<"cluster disconnect "<<conn->host<<" "<<conn->port;
						return false;
					}
				
				}
			}

			zfree(o);
		}
	}
	
	
	
	redis->clusterRepliMigratEnabled = false;
	
	{
		bool mark = false;
		{
			std::unique_lock <std::mutex> lck(redis->clusterMutex);
			for(auto it = tcpvectors.begin(); it != tcpvectors.end(); it ++)
			{
				if((*it)->host == ip && (*it)->port == port)
			 	{
			 		(*it)->setMessageCallback(std::bind(&xCluster::readCallBack, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
					mark = true;
			 	}
			}
		}

		if(!mark)
		{
			LOG_INFO<<"tcpclient not found "<<ip<<" "<<port;
			return false;
		}

		std::string hp = redis->host +"::" +  std::to_string(redis->port);
		std::deque<rObj*> robj;
		robj.push_back(createStringObject("cluster", 7));
		robj.push_back(createStringObject("setslot", 7));
		robj.push_back(createStringObject("node", 4));
		robj.push_back(createStringObject((char*)hp.c_str(),hp.length()));
		robj.push_back(createStringObject((char*)ipPort.c_str(),ipPort.length()));
			
		for(auto it = uset.begin(); it != uset.end(); it ++)
		{
			std::unique_lock <std::mutex> lck(redis->clusterMutex);
			char buf[4];
			int len = ll2string(buf, sizeof(buf), *it);
			robj.push_back(createStringObject(buf, len));
		}	
		
		syncClusterSlot(robj);	

	}

	

	LOG_INFO<<"cluster send  success";
	
}


void xCluster::connCallBack(const xTcpconnectionPtr& conn, void *data)
{
	if (conn->connected())
	{
		isConnect = true;
		state = false;
		{
			std::unique_lock <std::mutex> lck(cmtex);
			condition.notify_one();
		}

		socket.getpeerName(conn->getSockfd(), &(conn->host), conn->port);
		xBuffer sendBuf;
		std::deque<rObj*> robj;
		rObj * c = createStringObject("cluster", 7);
		rObj * m = createStringObject("connect", 7);
		robj.push_back(c);
		robj.push_back(m);
		
		{
			std::unique_lock <std::mutex> lck(redis->clusterMutex);
			for (auto it = redis->clustertcpconnMaps.begin(); it != redis->clustertcpconnMaps.end(); it++)
			{
				structureProtocolSetCluster(it->second->host, it->second->port, sendBuf, robj,conn);
			}

			structureProtocolSetCluster(redis->host, redis->port, sendBuf, robj,conn);
			for (auto it = robj.begin(); it != robj.end(); ++it)
			{
				zfree(*it);
			}

			redis->clustertcpconnMaps.insert(std::make_pair(conn->getSockfd(), conn));
		}

		std::shared_ptr<xSession> session(new xSession(redis, conn));
		{
			std::unique_lock <std::mutex> lck(redis->mtx);
			redis->sessions[conn->getSockfd()] = session;
		}

		LOG_INFO << "connect cluster success "<<"ip:"<<conn->host<<" port:"<<conn->port;

	}
	else
	{
		{
			std::unique_lock <std::mutex> lck(redis->clusterMutex);
			redis->clustertcpconnMaps.erase(conn->getSockfd());
			for (auto it = tcpvectors.begin(); it != tcpvectors.end(); it++)
			{
				if ((*it)->connection->host == conn->host && (*it)->connection->port == conn->port)
				{
					tcpvectors.erase(it);
					break;
				}
			} 
			eraseClusterNode(conn->host,conn->port);
			migratingSlosTos.erase(conn->host + std::to_string(conn->port));
			importingSlotsFrom.erase(conn->host + std::to_string(conn->port));

		}

		LOG_INFO << "disconnect cluster "<<"ip:"<<conn->host<<" port:"<<conn->port;
	}
}



void xCluster::getKeyInSlot(int hashslot,rObj **keys,int count)
{
	int j = 0;
	for(auto it = redis->setMapShards.begin(); it != redis->setMapShards.end();it++)
	{
		std::unique_lock <std::mutex> lck((*it).mtx);
		for(auto iter = (*it).setMap.begin(); iter !=  (*it).setMap.end(); iter ++)
		{
			if(count ==0)
			{
				return ;
			}
			
			unsigned int slot = keyHashSlot((char*)iter->first->ptr,sdslen(iter->first->ptr));
			if(slot == hashslot)
			{
				keys[j++] = iter->first;
				count --;
			}
			
		}
	}
	
}


void xCluster::eraseImportSlot(int slot)
{

}

void xCluster::eraseClusterNode(std::string host,int32_t port)
{
	for(auto it = clusterSlotNodes.begin(); it != clusterSlotNodes.end(); )
	{
		if(host == it->second.ip && port == it->second.port)
		{
			clusterSlotNodes.erase(it++);
			continue;
		}

		it ++;
	}

}


bool  xCluster::connSetCluster(std::string ip, int32_t port)
{
	std::shared_ptr<xTcpClient> client(new xTcpClient(loop, this));
	client->setConnectionCallback(std::bind(&xCluster::connCallBack, this, std::placeholders::_1, std::placeholders::_2));
	client->setMessageCallback(std::bind(&xCluster::readCallBack, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
	client->setConnectionErrorCallBack(std::bind(&xCluster::connErrorCallBack, this));
	client->connect(ip.c_str(), port);
	tcpvectors.push_back(client);

	std::unique_lock <std::mutex> lck(cmtex);
	while(state)
	{
		condition.wait(lck);
	}
	
	state = true;

	if(isConnect)
	{
		isConnect = false;
		return true;
	}
	else
	{
		return false;
	}
}

void xCluster::connectCluster()
{
	xEventLoop loop;
	this->loop = &loop;
	loop.run();
}

void xCluster::reconnectTimer(void * data)
{
	LOG_INFO << "Reconnect..........";
}

void xCluster::connErrorCallBack()
{
	state = false;
	isConnect = false;

	{
		std::unique_lock <std::mutex> lck(cmtex);
		condition.notify_one();
	}
}

void xCluster::init(xRedis * redis)
{
	this->redis = redis;
}




