#include "util/mysqlconnector.h"
#include <boost/asio/io_context.hpp>
#include <spdlog/spdlog.h>

MySQLConnector::MySQLConnector()
{
    try
    {
        // 创建一个专用的 io_context 用于 MySQL 连接池
        static boost::asio::io_context mysql_ioc;

        boost::mysql::pool_params params;
        params.server_address.emplace_host_and_port("127.0.0.1", 3306);
        params.username = "ape_user";
        params.password = "ape_password";
        params.database = "ape_auth";
        // 默认 SSL 模式为 enable，在本地开发环境中使用 disable
        params.ssl = boost::mysql::ssl_mode::disable;
        params.initial_size = 2;
        params.max_size = 20;

        pool_ = std::make_unique<boost::mysql::connection_pool>(
            boost::asio::any_io_executor(mysql_ioc.get_executor()),
            std::move(params));

        // 启动 io_context 在后台线程
        static std::thread mysql_thread([&]() {
            try
            {
                // 必须先调用 async_run() 启动连接池
                pool_->async_run(boost::asio::detached);
                mysql_ioc.run();
            }
            catch (const std::exception &e)
            {
                spdlog::error("MySQL io_context error: {}", e.what());
            }
        });
        mysql_thread.detach();

        spdlog::info("MySQL connection pool created successfully");
    }
    catch (const std::exception &e)
    {
        spdlog::error("Failed to create MySQL connection pool: {}", e.what());
        throw;
    }
}

MySQLConnector &MySQLConnector::get_instance()
{
    static MySQLConnector instance;
    return instance;
}