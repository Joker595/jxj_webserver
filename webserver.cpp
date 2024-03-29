#include "webserver.h"

WebServer::WebServer() {
    // http_conn类对象
    users = new http_conn[MAX_FD];

    // root文件夹路径
    char server_path[200];
    // 获取当前绝对工作路径
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 定时器
    users_timer = new client_data[MAX_FD];
}

// 析构函数，释放所有内存
WebServer::~WebServer() {
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    // 注意使用delete[]
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

// 参数初始化
void
WebServer::init(int port,
                string user,
                string passWord,
                string databaseName,
                int log_write,
                int opt_linger,
                int trigmode,
                int sql_num,
                int thread_num,
                int close_log,
                int actor_model) {
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

// 初始化trig_mode
void
WebServer::trig_mode() {
    // LT + LT
    if (0 == m_TRIGMode) {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    // LT + ET
    else if (1 == m_TRIGMode) {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    // ET + LT
    else if (2 == m_TRIGMode) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    // ET + ET
    else if (3 == m_TRIGMode) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

// 初始化日志
void
WebServer::log_write() {
    if (0 == m_close_log) {
        // 初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

// 初始化数据库
void
WebServer::sql_pool() {
    // 初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void
WebServer::thread_pool() {
    // 线程池（注意需要先初始化数据库连接）
    // 这里已经初始化并启动了线程数组
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void
WebServer::eventListen() {
    // 网络编程基础步骤
    // PF_INET和AF_INET等价
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // 优雅关闭连接
    if (0 == m_OPT_LINGER) {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    } else if (1 == m_OPT_LINGER) {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    // 可以使用memset
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    // 监听任意端口（注意转换字节序）
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    // 设置快速重用，对于服务器崩溃后恢复重用同一端口比较有效
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    // utils是空的默认构造函数创建的
    utils.init(TIMESLOT);

    // epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    // 注册监听套接字事件
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    // NOTE:创建全双工套接字对用于进程/线程间通信
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);

    // m_pipefd[1]用于写
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    // NOTE：SIGPIPE写已关闭管道时会收到；
    // NOTE：SIG_IGN忽略信号
    // 该行将优雅处理“管道破裂”情况
    utils.addsig(SIGPIPE, SIG_IGN);
    // 这里sig_handler通过类实例的.操作符访问
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    // 定时闹钟，单位s
    alarm(TIMESLOT);

    // 工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

// 初始化http_conn连接以及相应的定时器
void
WebServer::timer(int connfd, struct sockaddr_in client_address) {
    // 直接用connfd作为索引访问users，初始化该连接（包括加入epoll监听事件等）
    users[connfd].init(connfd,
                       client_address,
                       m_root,
                       m_CONNTrigmode,
                       m_close_log,
                       m_user,
                       m_passWord,
                       m_databaseName);

    // 初始化client_data数据(和users通过下标一一对应)
    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    // 互相存有指针
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    // 设置超时时间
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

// 若有数据传输，则将定时器往后延迟3个单位
// 并对新的定时器在链表上的位置进行调整
void
WebServer::adjust_timer(util_timer *timer) {
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

// 关闭sockfd的连接并清理定时器
void
WebServer::deal_timer(util_timer *timer, int sockfd) {
    // 关闭客户连接
    timer->cb_func(&users_timer[sockfd]);
    if (timer) {
        // 关闭对应的定时器
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

// 处理新来的客户端连接
bool
WebServer::dealclinetdata() {
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    // LT模式（简单），listenfd一就绪就触发
    if (0 == m_LISTENTrigmode) {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0) {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        // 超出最大连接数则关闭连接并返回错误
        if (http_conn::m_user_count >= MAX_FD) {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    // ET模式（复杂），仅在状态由不就绪转变为就绪触发一次
    else {
        while (1) {
            // 问题：m_listenfd是不是需要被设置为非阻塞的
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0) {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD) {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

// 解析m_pipefd[0]收到的信号并处理
bool
WebServer::dealwithsignal(bool &timeout, bool &stop_server) {
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1) {
        return false;
    } else if (ret == 0) {
        return false;
    } else {
        // 每个信号被当做一个char，即仅有一个字节
        for (int i = 0; i < ret; ++i) {
            switch (signals[i]) {
                case SIGALRM: {
                    timeout = true;
                    break;
                }
                case SIGTERM: {
                    stop_server = true;
                    break;
                }
            }
        }
    }
    return true;
}

// 处理读事件，将事件放入请求队列
void
WebServer::dealwithread(int sockfd) {
    // 根据sockfd索引
    util_timer *timer = users_timer[sockfd].timer;

    // reactor
    if (1 == m_actormodel) {
        if (timer) {
            adjust_timer(timer);
        }

        // 若监测到读事件，将该事件放入请求队列(同样用sockfd索引)
        m_pool->append(users + sockfd, 0);

        while (true) {
            // 等待线程池通知：已完成任务
            if (1 == users[sockfd].improv) {
                if (1 == users[sockfd].timer_flag) {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    } else {
        // proactor
        if (users[sockfd].read_once()) {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            // 若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer) {
                adjust_timer(timer);
            }
        } else {
            deal_timer(timer, sockfd);
        }
    }
}

void
WebServer::dealwithwrite(int sockfd) {
    util_timer *timer = users_timer[sockfd].timer;
    // reactor
    if (1 == m_actormodel) {
        if (timer) {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true) {
            // 监测该任务是否完成，若完成则退出循环
            if (1 == users[sockfd].improv) {
                if (1 == users[sockfd].timer_flag) {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    } else {
        // proactor
        if (users[sockfd].write()) {
            LOG_INFO("send data to the client(%s)",
                     inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer) {
                adjust_timer(timer);
            }
        } else {
            deal_timer(timer, sockfd);
        }
    }
}

// 主线程函数
void
WebServer::eventLoop() {
    // 是否超时
    bool timeout = false;
    // 是否关闭server
    bool stop_server = false;

    while (!stop_server) {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) {
            // epoll出现非中断的错误
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            // 处理新到的客户连接
            if (sockfd == m_listenfd) {
                bool flag = dealclinetdata();
                // 可能是ET模式下返回值为失败
                if (false == flag) continue;
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {  // 关闭事件
                // 服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            // 处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag) LOG_ERROR("%s", "dealsignal failure");
            }
            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN) {
                dealwithread(sockfd);
            } else if (events[i].events & EPOLLOUT) {
                dealwithwrite(sockfd);
            }
        }
        // 定期清理过期的连接
        if (timeout) {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}