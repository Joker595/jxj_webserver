#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "../CGImysql/sql_connection_pool.h"
#include "../lock/locker.h"

#include <cstdio>
#include <exception>
#include <list>
#include <pthread.h>

template <typename T>
class threadpool {
   public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model,
               connection_pool *connPool,
               int thread_number = 8,
               int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

   private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

   private:
    int m_thread_number;          // 线程池中的线程数
    int m_max_requests;           // 请求队列中允许的最大请求数
    pthread_t *m_threads;         // 描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue;   // 请求队列
    locker m_queuelocker;         // 保护请求队列的互斥锁
    sem m_queuestat;              // 是否有任务需要处理
    connection_pool *m_connPool;  // 数据库
    int m_actor_model;            // 模型切换
};
template <typename T>
threadpool<T>::threadpool(int actor_model,
                          connection_pool *connPool,
                          int thread_number,
                          int max_requests)
    : m_actor_model(actor_model),
      m_thread_number(thread_number),
      m_max_requests(max_requests),
      m_threads(NULL),
      m_connPool(connPool) {
    if (thread_number <= 0 || max_requests <= 0) throw std::exception();
    // 建立线程数组
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) throw std::exception();
    // 创建thread_number个线程并分离（分离后线程标识符仍然有效）
    for (int i = 0; i < thread_number; ++i) {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i])) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
}

template <typename T>
bool
threadpool<T>::append(T *request, int state) {
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    // 放入request后通过信号量通知
    m_queuestat.post();
    return true;
}

template <typename T>
bool
threadpool<T>::append_p(T *request) {
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
void *
threadpool<T>::worker(void *arg) {
    // 通过void* arg获取参数threadpool*（worker是static函数，没有隐式的this指针）
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void
threadpool<T>::run() {
    while (true) {
        // 等待信号量
        m_queuestat.wait();
        m_queuelocker.lock();
        // 判断工作队列是否有任务
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        // request是一个http_conn
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        // 工作队列解锁，任务之间不存在冲突
        m_queuelocker.unlock();
        if (!request) continue;
        // Reactor模式
        if (1 == m_actor_model) {
            // m_state=0为读
            if (0 == request->m_state) {
                if (request->read_once()) {
                    // 这里修改了improv之后主线程会继续运行
                    // 即等待该工作线程读取完数据
                    // 猜测是为了等待读取的结果看是否需要关闭连接
                    request->improv = 1;
                    // 这里应该是&(request->mysql)
                    // NOTE:此处建立了数据库连接
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    // 核心处理逻辑
                    request->process();
                } else {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            } else {
                // m_state=1为写
                if (request->write()) {
                    request->improv = 1;
                } else {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            // Proactor模式
        } else {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif
