#include "messagequeue/kafkaproducer.h"
#include <librdkafka/rdkafkacpp.h>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <atomic>
#include <memory>
#include <chrono>
#include <stdexcept>

namespace {
struct DeliveryState
{
    std::atomic<int> status{0};
};
}

void ProducerDeliveryReportCb::dr_cb(RdKafka::Message& message) {
    if (auto* opaque = static_cast<std::shared_ptr<DeliveryState>*>(message.msg_opaque())) {
        (*opaque)->status.store(message.err() == RdKafka::ERR_NO_ERROR ? 1 : 2);
        delete opaque;
    }
    // 处理生产者投递回调
    if(message.err() != RdKafka::ERR_NO_ERROR){
        spdlog::error("Producer delivery failed: {}", message.errstr());
    }else {
        spdlog::info("Producer delivery success: topic = {}, partition = {}, offset = {}", message.topic_name(), message.partition(), message.offset());
    }
}

void ProducerEventCb::event_cb(RdKafka::Event& event) {
    // 处理生产者事件回调
    switch (event.type()) {
        case RdKafka::Event::EVENT_ERROR:
            spdlog::error("Producer Error: {}", RdKafka::err2str(event.err()));
            break;
        case RdKafka::Event::EVENT_STATS:
            spdlog::info("Producer Stats: {}", event.str());
            break;
        case RdKafka::Event::EVENT_LOG:
            spdlog::info("Producer Log: {}", event.str());
            break;
        case RdKafka::Event::EVENT_THROTTLE:
            spdlog::info("Producer Throttle: {}, {}", event.broker_name(), event.broker_id()); 
            break;
        default:
            spdlog::info("Producer Event: {}", event.str());
            break;
    }
}

int32_t ProducerPartitionStrategyCb::partitioner_cb(const RdKafka::Topic *topic,
                                     const std::string *key,
                                     int32_t partition_cnt,
                                     void *msg_opaque) {
    // 处理生产者分区策略回调
    std::hash<std::string> strhasher;
    size_t hash_value = strhasher(*key);
    return hash_value % partition_cnt;
}

KafkaProducer::KafkaProducer( std::string topicName, std::string brokerList)
    : globalConfig(nullptr), topicConfig(nullptr), drCb(nullptr), eventCb(nullptr),
      partitionerCb(nullptr), producerInstance(nullptr), topic(nullptr)
{
    this->brokerList = brokerList;
    this->topicName = topicName;
    this->drCb = new ProducerDeliveryReportCb();
    this->eventCb = new ProducerEventCb();
    this->partitionerCb = new ProducerPartitionStrategyCb();


    RdKafka::Conf::ConfResult errCode;
    std::string errorStr;                 

    // 创建全局、Topic配置对象（用于生成相应的参数对象）
    globalConfig = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
    if(!globalConfig){
        spdlog::error("KafkaProducer: Failed to create globalConfig");
        return;
    }
    topicConfig = RdKafka::Conf::create(RdKafka::Conf::CONF_TOPIC);
    if(!topicConfig){
        spdlog::error("KafkaProducer: Failed to create topicConfig");
        return;
    }


    // 设置broker
    errCode = globalConfig->set("bootstrap.servers", brokerList, errorStr);
    if (errCode != RdKafka::Conf::CONF_OK) {
        spdlog::error("KafkaProducer: Failed to set bootstrap.servers: {}", errorStr);
        return;
    }
    if (globalConfig->set("enable.idempotence", "true", errorStr) != RdKafka::Conf::CONF_OK ||
        globalConfig->set("delivery.timeout.ms", "10000", errorStr) != RdKafka::Conf::CONF_OK)
        throw std::runtime_error("Kafka reliability configuration failed: " + errorStr);

    //设置生产者投递回调
    errCode = globalConfig->set("dr_cb", drCb, errorStr);
    if (errCode != RdKafka::Conf::CONF_OK) {
        spdlog::error("KafkaProducer: Failed to set dr_cb: {}", errorStr);
        return;
    }

    //设置生产者系统级事件回调
    errCode = globalConfig->set("event_cb", eventCb, errorStr);   
    if (errCode != RdKafka::Conf::CONF_OK) {
        spdlog::error("KafkaProducer: Failed to set event_cb: {}", errorStr);
        return;
    }

    //设置Topic的分区策略回调
    errCode = topicConfig->set("partitioner_cb", partitionerCb, errorStr);
    if (errCode != RdKafka::Conf::CONF_OK) {
        spdlog::error("KafkaProducer: Failed to set partitioner_cb: {}", errorStr);
        return;
    }

    // 创建生产者实例
    producerInstance = RdKafka::Producer::create(globalConfig, errorStr);
    if (!producerInstance) {
        spdlog::error("KafkaProducer: Failed to create producer: {}", errorStr);
        return;
    }
    // 设置默认Topic配置
    topic = RdKafka::Topic::create(producerInstance, topicName, topicConfig, errorStr);
    if (!topic) {
        spdlog::error("KafkaProducer: Failed to create topic: {}", errorStr);
        return;
    }

}


bool KafkaProducer::deliver(std::string& key, void* payload, size_t payloadSize)
{
    if (!producerInstance || !topic)
        return false;
    // 投递消息
    RdKafka::ErrorCode errCode = producerInstance->produce(
        topic,
        RdKafka::Topic::PARTITION_UA,
        RdKafka::Producer::RK_MSG_COPY,
        payload,
        payloadSize,
        &key,
        nullptr
    );
    producerInstance->poll(0);
    if (errCode != RdKafka::ERR_NO_ERROR) {
        spdlog::error("KafkaProducer: Failed to produce message: {}", RdKafka::err2str(errCode));
        if(errCode == RdKafka::ERR__QUEUE_FULL){
            producerInstance->poll(100);
            return false;
        }
        return false;
    }
    return true;

}

boost::asio::awaitable<bool> KafkaProducer::deliverConfirmed(std::string key, std::string payload)
{
    if (!producerInstance || !topic)
        co_return false;
    auto state = std::make_shared<DeliveryState>();
    auto* opaque = new std::shared_ptr<DeliveryState>(state);
    const auto error = producerInstance->produce(topic, RdKafka::Topic::PARTITION_UA,
        RdKafka::Producer::RK_MSG_COPY, payload.data(), payload.size(), &key, opaque);
    if (error != RdKafka::ERR_NO_ERROR) {
        delete opaque;
        co_return false;
    }
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (state->status.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        producerInstance->poll(0);
        if (state->status.load() != 0)
            break;
        timer.expires_after(std::chrono::milliseconds(50));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
    co_return state->status.load() == 1;
}



KafkaProducer::~KafkaProducer()
{
    // 销毁生产者实例
    if (producerInstance) {
        producerInstance->flush(10000); // 等待所有消息发送完成
        delete producerInstance;
    }
    // 销毁Topic对象
    if (topic) {
        delete topic;
    }
    // 销毁Topic配置对象
    if (topicConfig) {
        delete topicConfig;
    }
    // 销毁全局配置对象
    if (globalConfig) {
        delete globalConfig;
    }
    // 销毁回调对象
    if (drCb) {
        delete drCb;
    }
    if (eventCb) {
        delete eventCb;
    }
    if (partitionerCb) {
        delete partitionerCb;
    }
}
