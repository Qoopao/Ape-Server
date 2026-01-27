#ifndef CONNMANAGER_H
#define CONNMANAGER_H

#include "gateway/session.h"
#include <boost/asio.hpp>
#include <unordered_map>

// 单例模式
class ConnManager
{
private:
    ConnManager() = default;
    ~ConnManager() = default;

    std::unordered_map<std::string, std::shared_ptr<Session>> conn_map = {};

public:
    // 禁止拷贝构造，禁止赋值
    ConnManager(const ConnManager &) = delete;
    ConnManager &operator=(const ConnManager &) = delete;

    // 获取单例
    static ConnManager &get_instance();

    // 添加连接
    void add_conn(std::string userid, std::shared_ptr<Session> session);

    // 移除连接
    void rmv_conn(std::string userid);

    // 获取连接
    std::shared_ptr<Session> get_conn(std::string userid);

    //获取用户-连接map
    const std::unordered_map<std::string, std::shared_ptr<Session>>& get_map();
};

#endif