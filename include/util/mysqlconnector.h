#ifndef MYSQLCONNECTOR_H
#define MYSQLCONNECTOR_H

#include <boost/mysql.hpp>
#include <memory>
#include <spdlog/spdlog.h>

class MySQLConnector
{
private:
    MySQLConnector();
    ~MySQLConnector() = default;

    std::unique_ptr<boost::mysql::connection_pool> pool_;

public:
    MySQLConnector &operator=(const MySQLConnector &rc) = delete;
    MySQLConnector(const MySQLConnector &rc) = delete;

    static MySQLConnector &get_instance();
    boost::mysql::connection_pool &get_pool() { return *pool_; }
};

#endif