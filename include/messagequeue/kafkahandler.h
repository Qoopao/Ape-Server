#ifndef KAFKAHANDLER_H
#define KAFKAHANDLER_H

#include <string>

// 消息处理器基类：定义消息处理的接口，子类实现具体业务逻辑
class KafkaHandler {
public:
    // 虚函数：处理Kafka消息（topic=主题，msg=消息内容）
    virtual void handle(const std::string& topic, const std::string& msg) = 0;

    // 虚析构
    virtual ~KafkaHandler() = default;
};

#endif