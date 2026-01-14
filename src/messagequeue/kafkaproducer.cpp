#include "messagequeue/kafkaproducer.h"
#include <librdkafka/rdkafkacpp.h>

void ProducerDeliveryReportCb::dr_cb(RdKafka::Message& message) {
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

KafkaProducer::KafkaProducer(std::string& brokerList, std::string& topicName)
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


void KafkaProducer::Deliver(std::string& key, void* payload, size_t payloadSize)
{
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
        }
    }
    
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
