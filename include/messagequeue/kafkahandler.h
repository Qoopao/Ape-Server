#ifndef KAFKAHANDLER_H
#define KAFKAHANDLER_H

#include <string>

// 消息处理器基类：定义消息处理的接口，子类实现具体业务逻辑
class KafkaHandler {
public:
    // 虚函数：处理Kafka消息（topic=主题，msg=消息内容）
    virtual void handle(const std::string& topic, const std::string& msg) = 0;

    // 虚析构函数（基类必须，避免内存泄漏）
    virtual ~KafkaHandler() = default;
};

// 示例：默认消息处理器（可以根据业务需求自定义子类）
class DefaultHandler : public KafkaHandler {
public:
    void handle(const std::string& topic, const std::string& msg) override;
};

#endif