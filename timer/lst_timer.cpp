#include "lst_timer.h"

#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst() {
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst() {
    util_timer *tmp = head;
    while (tmp) {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

// 添加timer进链表
void
sort_timer_lst::add_timer(util_timer *timer) {
    if (!timer) {
        return;
    }
    // 注意判断此时链表空的情况
    if (!head) {
        head = tail = timer;
        return;
    }
    // timer的链表较head更早
    if (timer->expire < head->expire) {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    // 其他链表情况
    add_timer(timer, head);
}

// 调整timer（似乎操作是增加expire）
void
sort_timer_lst::adjust_timer(util_timer *timer) {
    if (!timer) {
        return;
    }
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire)) {
        return;
    }
    if (timer == head) {
        // 更改头结点
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    } else {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);  // 该函数不会修改头结点
    }
}
void
sort_timer_lst::del_timer(util_timer *timer) {
    if (!timer) {
        return;
    }
    // 链表中仅有timer
    if ((timer == head) && (timer == tail)) {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail) {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// 清理过期的连接
void
sort_timer_lst::tick() {
    if (!head) {
        return;
    }

    time_t cur = time(NULL);
    util_timer *tmp = head;
    while (tmp) {
        if (cur < tmp->expire) {
            break;
        }
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if (head) {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

void
sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head) {
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp) {
        if (timer->expire < tmp->expire) {
            // 链表中间找到插入点
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp) {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void
Utils::init(int timeslot) {
    m_TIMESLOT = timeslot;
}

// 对文件描述符设置非阻塞
int
Utils::setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void
Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot) event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数
// NOTE：安全处理信号，仅在信号处理函数中执行最小操作，即把信号通过管道传递给其他线程处理
void
Utils::sig_handler(int sig) {
    // 为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    // 信号类型一般不超过255，可以只发送第一个字节（n=1），减少开销
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 设置信号函数
// void(handler)(int) 和 void(*handler)(int)是等价的，都是函数指针
void
Utils::addsig(int sig, void(handler)(int), bool restart) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart) sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    // 更改接收信号的行为（由sa指定）
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void
Utils::timer_handler() {
    m_timer_lst.tick();
    // alarm()  arranges  for a SIGALRM signal to be delivered to the calling process in
    // seconds seconds.
    alarm(m_TIMESLOT);
}

// 直接向connfd发送错误信息
void
Utils::show_error(int connfd, const char *info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;

// 关闭该客户连接
void
cb_func(client_data *user_data) {
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
