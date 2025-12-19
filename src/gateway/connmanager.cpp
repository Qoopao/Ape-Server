#include "gateway/connmanager.h"
#include <boost/asio.hpp>
#include <unordered_map>
#include <spdlog/spdlog.h>

ConnManager &ConnManager::get_instance()
{
    static ConnManager instance;
    return instance;
}

void ConnManager::add_conn(std::string userid, std::shared_ptr<Session> session)
{
    if (conn_map.find(userid) == conn_map.end())
    {
        conn_map.emplace(userid, session);
    }
    else
    {
        spdlog::error("ConnManager error: add_conn");
    }
}

void ConnManager::rmv_conn(std::string userid)
{
    if (conn_map.find(userid) != conn_map.end())
    {
        conn_map.erase(userid);
    }
    else
    {
        spdlog::error("ConnManager error: rmv_conn");
    }
}

std::shared_ptr<Session> ConnManager::get_conn(std::string userid)
{
    auto it = conn_map.find(userid);
    if (it == conn_map.end())
    {
        spdlog::error("ConnManager error: get_conn");
    }
    return it->second;
}

const std::unordered_map<std::string, std::shared_ptr<Session>> &ConnManager::get_map()
{
    return conn_map;
}