#ifndef KAFKACONSUMER_H
#define KAFKACONSUMER_H
    

#include <librdkafka/rdkafkacpp.h>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

// 消费者系统级事件回调函数
class ConsumerEventCb : public RdKafka::EventCb{
    void event_cb(RdKafka::Event& event) override;
};

class ConsumerRebalanceCb : public RdKafka::RebalanceCb{
    void rebalance_cb(RdKafka::KafkaConsumer *consumer,
                            RdKafka::ErrorCode err,
                            std::vector<RdKafka::TopicPartition *> &partitions) override;
};

// Kafka消费者类：依赖KafkaConnector和MsgHandler，实现消息消费
class KafkaConsumer
{
public:
    // 构造函数：初始化消费者（connector=连接配置，topic=订阅的主题，handler=消息处理器）
    KafkaConsumer(const std::string& brokerList,
                  const std::string& groupId, 
                  const std::vector<std::string>& topicNames);

    // 消费消息
    void consumeMsg(RdKafka::Message& msg, void *opaque);
                  
    // 启动消费
    void start();



    // 析构函数：释放资源
    ~KafkaConsumer();

private:
    // 禁止拷贝和赋值
    KafkaConsumer(const KafkaConsumer &) = delete;
    KafkaConsumer &operator=(const KafkaConsumer &) = delete;

    std::string brokerList;
    std::string groupId;
    std::vector<std::string> topicNames;
    int partition;

    RdKafka::Conf *globalConfig;
    RdKafka::Conf *topicConfig;
    ConsumerEventCb *eventCb;
    ConsumerRebalanceCb *rebalanceCb;

    RdKafka::KafkaConsumer *consumerInstance;

};

#endif