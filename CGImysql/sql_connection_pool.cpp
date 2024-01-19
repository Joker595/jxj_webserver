#include "sql_connection_pool.h"

#include <iostream>
#include <list>
#include <mysql/mysql.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

using namespace std;

connection_pool::connection_pool() {
    m_CurConn = 0;
    m_FreeConn = 0;
}

// 懒汉模式，c++11保证static变量多线程安全
connection_pool *
connection_pool::GetInstance() {
    static connection_pool connPool;
    return &connPool;
}

// 构造初始化
void
connection_pool::init(string url,
                      string User,
                      string PassWord,
                      string DBName,
                      int Port,
                      int MaxConn,
                      int close_log) {
    m_url = url;
    m_Port = Port;
    m_User = User;
    m_PassWord = PassWord;
    m_DatabaseName = DBName;
    m_close_log = close_log;

    for (int i = 0; i < MaxConn; i++) {
        MYSQL *con = NULL;
        // 连接数据库前初始化MYSQL指针
        con = mysql_init(con);

        if (con == NULL) {
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        // 建立实际连接
        con = mysql_real_connect(con,
                                 url.c_str(),
                                 User.c_str(),
                                 PassWord.c_str(),
                                 DBName.c_str(),
                                 Port,
                                 NULL,
                                 0);

        if (con == NULL) {
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        connList.push_back(con);
        ++m_FreeConn;
    }

    // 创建初始值为m_FreeConn的信号量（信号量用于控制并发线程数量）
    reserve = sem(m_FreeConn);

    // m_MaxConn取值为成功建立的最大连接数
    m_MaxConn = m_FreeConn;
}

// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *
connection_pool::GetConnection() {
    MYSQL *con = NULL;

    // 多线程仅读操作是线程安全的，所以这句不需要加锁
    if (0 == connList.size()) return NULL;

    // 封装的sem_wait函数
    reserve.wait();

    // 信号量仅用于控制并发数量，还需要加锁保证操作互斥性
    lock.lock();

    con = connList.front();
    connList.pop_front();

    // 信号量sem已经-1，m_FreeConn需要同时减1
    --m_FreeConn;
    ++m_CurConn;

    lock.unlock();
    return con;
}

// 释放当前使用的连接
bool
connection_pool::ReleaseConnection(MYSQL *con) {
    if (NULL == con) return false;

    lock.lock();

    connList.push_back(con);
    ++m_FreeConn;
    --m_CurConn;

    lock.unlock();

    // 使用完毕还原信号量
    reserve.post();
    return true;
}

// 销毁数据库连接池
void
connection_pool::DestroyPool() {
    lock.lock();
    if (connList.size() > 0) {
        list<MYSQL *>::iterator it;
        for (it = connList.begin(); it != connList.end(); ++it) {
            // NOTE：不能直接销毁it，否则it会变为nullptr
            MYSQL *con = *it;
            mysql_close(con);
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        connList.clear();
    }

    lock.unlock();
}

// 当前空闲的连接数
int
connection_pool::GetFreeConn() {
    // 不需要加锁？
    return this->m_FreeConn;
}

// RAII（Resource Acquisition Is
// Initialization）思想，方便管理，资源的获取与释放与类的构造和析构绑定
connection_pool::~connection_pool() { DestroyPool(); }

// MYSQL **SQL双重指针，目的是在函数内修改指针的值
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool) {
    *SQL = connPool->GetConnection();

    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII() { poolRAII->ReleaseConnection(conRAII); }