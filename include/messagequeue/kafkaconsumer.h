#ifndef KAFKACONSUMER_H
#define KAFKACONSUMER_H
    

#include <atomic>
#include <librdkafka/rdkafkacpp.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>
#include <memory>

class KafkaHandler;

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
    KafkaConsumer(const std::string& groupId, 
                  const std::vector<std::string>& topicNames,
                  const std::string& brokerList="localhost:29092,localhost:39092,localhost:49092");

    // 设置消息处理器
    void setHandler(std::shared_ptr<KafkaHandler> handler);

    // 消费消息
    void consumeMsg(RdKafka::Message& msg, void *opaque);
                  
    // 启动消费（阻塞当前线程）
    void start();

    // 优雅停止消费（设置标志，下次 poll 返回时退出循环）
    void shutdown();

    // 等待消费线程退出
    void join();

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

    std::shared_ptr<KafkaHandler> msgHandler;

    std::atomic<bool> running_{true};
    std::thread consume_thread_;
};

#endif