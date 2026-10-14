#ifndef KAFKACONSUMER_H
#define KAFKACONSUMER_H

#include "messagequeue/message_consumer.h"
#include "messagequeue/message_handler.h"
#include <atomic>
#include <librdkafka/rdkafkacpp.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>
#include <memory>
#include <deque>
#include <map>
#include <mutex>
#include <future>
#include <boost/asio/awaitable.hpp>

class IOC_Pool;

// 消费者系统级事件回调函数
class ConsumerEventCb : public RdKafka::EventCb{
    void event_cb(RdKafka::Event& event) override;
};

class ConsumerRebalanceCb : public RdKafka::RebalanceCb{
public:
    explicit ConsumerRebalanceCb(std::atomic<uint64_t>& generation) : generation_(generation) {}
private:
    std::atomic<uint64_t>& generation_;
    void rebalance_cb(RdKafka::KafkaConsumer *consumer,
                            RdKafka::ErrorCode err,
                            std::vector<RdKafka::TopicPartition *> &partitions) override;
};

// Kafka消费者类：实现 IMessageConsumer 接口
class KafkaConsumer : public IMessageConsumer
{
public:
    // 构造函数：初始化消费者（connector=连接配置，topic=订阅的主题，handler=消息处理器）
    KafkaConsumer(const std::string& groupId,
                  const std::vector<std::string>& topicNames,
                  const std::string& brokerList="localhost:29092,localhost:39092,localhost:49092");

    // 设置消息处理器（实现 IMessageConsumer 接口）
    void setHandler(std::shared_ptr<MessageHandler> handler) override;

    // 设置 IOC_Pool，Kafka 消费使用独立的线程池
    void setIOCPool(IOC_Pool *pool) override;

    // 消费消息
    void consumeMsg(RdKafka::Message& msg, void *opaque);

    // 启动消费（阻塞当前线程）
    void start() override;

    // 优雅停止消费（设置标志，下次 poll 返回时退出循环）
    void shutdown() override;

    // 等待消费线程退出
    void join() override;

    // 析构函数：释放资源
    ~KafkaConsumer();

private:
    struct Job
    {
        std::string topic, payload;
        int partition = 0;
        int64_t offset = 0;
        uint64_t generation = 0;
    };
    struct PartitionLane
    {
        std::mutex mutex;
        std::deque<Job> jobs;
        std::future<void> worker;
        bool paused = false;
        uint64_t pauseGeneration = 0;
    };
    boost::asio::awaitable<void> processPartition(std::shared_ptr<PartitionLane> lane);
    void updateBackpressure();
    std::atomic<uint64_t> generation_{0};
    std::mutex lanes_mutex_;
    std::map<std::pair<std::string, int>, std::shared_ptr<PartitionLane>> lanes_;
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

    std::shared_ptr<MessageHandler> msgHandler;
    IOC_Pool *ioc_pool_{nullptr};

    std::atomic<bool> running_{true};
    std::thread consume_thread_;

};

#endif
