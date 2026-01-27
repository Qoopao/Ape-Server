#ifndef KAFKAPRODUCER_H
#define KAFKAPRODUCER_H

#include <librdkafka/rdkafkacpp.h>
#include <spdlog/spdlog.h>
#include <string>

class ProducerDeliveryReportCb : public RdKafka::DeliveryReportCb{
    // 生产者投递回调函数
    void dr_cb(RdKafka::Message& message) override;
};

class ProducerEventCb : public RdKafka::EventCb{
    // 生产者系统级事件回调函数
    void event_cb(RdKafka::Event& event) override;
};

class ProducerPartitionStrategyCb : public RdKafka::PartitionerCb{
    // 生产者分区策略回调函数
    virtual int32_t partitioner_cb(const RdKafka::Topic *topic,
                                 const std::string *key,
                                 int32_t partition_cnt,
                                 void *msg_opaque) override;
};

// Kafka生产者类
class KafkaProducer
{
public:
    // 初始化生产者
    KafkaProducer(std::string& brokerList, std::string& topicName);

    // 投递消息
    void Deliver(std::string& key, void* payload, size_t payloadSize);

    // 析构函数：释放资源
    ~KafkaProducer();

private:
    // 禁止拷贝和赋值（避免资源重复释放）
    KafkaProducer(const KafkaProducer &) = delete;
    KafkaProducer &operator=(const KafkaProducer &) = delete;

    std::string brokerList; // Kafka broker列表
    std::string topicName;  // 默认主题名

    RdKafka::Conf *globalConfig; // 全局配置实例
    RdKafka::Conf *topicConfig; //  主题配置实例
    RdKafka::DeliveryReportCb *drCb;   // 投递回调
    RdKafka::EventCb *eventCb;         // 事件回调
    RdKafka::PartitionerCb *partitionerCb; // 分区回调

    RdKafka::Producer *producerInstance; // librdkafka生产者实例
    RdKafka::Topic *topic; // 主题实例
};

#endif
