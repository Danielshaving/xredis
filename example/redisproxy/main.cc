#include "redisproxy.h"

const char *logo =
        "                _._                                                  \n"
        "           _.-``__ ''-._                                             \n"
        "      _.-``    `.  `_.  ''-._           redis-proxy 1.0	    	  \n"
        "  .-`` .-```.  ```\\/    _.,_ ''-._                                  \n"
        " (    '      ,       .-`  | `,    )								  \n"
        " |`-._`-...-` __...-.``-._|'` _.-'|								  \n"
        " |    `-._   `._    /     _.-'    |  								  \n"
        "  `-._    `-._  `-./  _.-'    _.-'                                   \n"
        " |`-._`-._    `-.__.-'    _.-'_.-'|                                  \n"
        " |    `-._`-._        _.-'_.-'    |                   				  \n"
        "  `-._    `-._`-.__.-'_.-'    _.-'                                   \n"
        " |`-._`-._    `-.__.-'    _.-'_.-'|                                  \n"
        " |    `-._`-._        _.-'_.-'    |                                  \n"
        "  `-._    `-._`-.__.-'_.-'    _.-'                                   \n"
        "      `-._    `-.__.-'    _.-'                                       \n"
        "          `-._        _.-'                                           \n"
        "              `-.__.-'                                               \n";


std::unique_ptr <LogFile> logFile;

void dummyOutput(const char *msg, int len) {
    printf("%s", msg);
    logFile->append(msg, len);
    logFile->flush();
}

//./redis-cli --cluster create 127.0.0.1:7000 127.0.0.1:7001 127.0.0.1:7002 127.0.0.1:7003 127.0.0.1:7004 127.0.0.1:7005 --cluster-replicas 1
//./redis-benchmark -r 1000000 -n 2000000 -t get,set,lpush,lpop -q -P 16
//ulimit -c unlimited
int main(int argc, char *argv[]) {
    printf("%s\n", logo);
#ifdef _WIN64
    WSADATA wsaData;
    int32_t iRet = WSAStartup(MAKEWORD(2, 2), &wsaData);
    assert(iRet == 0);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    logFile.reset(new LogFile("log", "proxy", 65536, false));
    Logger::setOutput(dummyOutput);
    RedisProxy proxy("127.0.0.1", 6378, "127.0.0.1", 7000, 0, 1);
    proxy.run();
    return 0;
}
