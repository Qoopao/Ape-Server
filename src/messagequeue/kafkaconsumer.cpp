#include "messagequeue/kafkaconsumer.h"
#include <librdkafka/rdkafkacpp.h>



void ConsumerEventCb::event_cb(RdKafka::Event& event) {
    switch (event.type()) {
        case RdKafka::Event::EVENT_ERROR:
            spdlog::error("KafkaConsumer Error: {}", RdKafka::err2str(event.err()));
            break;
        case RdKafka::Event::EVENT_STATS:
            spdlog::info("KafkaConsumer Stats: {}", event.str());
            break;
        case RdKafka::Event::EVENT_LOG:
            spdlog::info("KafkaConsumer Log: {}", event.str());
            break;
        case RdKafka::Event::EVENT_THROTTLE:
            spdlog::info("KafkaConsumer Throttle: {}, {}", event.broker_name(), event.broker_id());
            break;
        default:
            spdlog::info("KafkaConsumer Event: {}", event.str());
            break;
    }
}

void ConsumerRebalanceCb::rebalance_cb(RdKafka::KafkaConsumer *consumer,
                            RdKafka::ErrorCode err,
                            std::vector<RdKafka::TopicPartition *> &partitions) {

    spdlog::info("KafkaConsumer Rebalance: {}", RdKafka::err2str(err));

    if(err == RdKafka::ERR__ASSIGN_PARTITIONS){
        consumer->assign(partitions);
    }else if(err == RdKafka::ERR__REVOKE_PARTITIONS){
        consumer->unassign();
    }
}

KafkaConsumer::KafkaConsumer(const std::string& brokerList,
                             const std::string& groupId, 
                             const std::vector<std::string>& topicNames) {
    this->brokerList = brokerList;
    this->groupId = groupId;
    this->topicNames = topicNames;
    this->eventCb = new ConsumerEventCb();
    this->rebalanceCb = new ConsumerRebalanceCb();

    RdKafka::Conf::ConfResult errCode;
    std::string errorStr;  

    // 创建全局配置对象
    globalConfig = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
    errCode = globalConfig->set("bootstrap.servers", brokerList, errorStr);
    if (errCode != RdKafka::Conf::CONF_OK) {
        spdlog::error("KafkaConsumer: Failed to set bootstrap.servers: {}", errorStr);
        return;
    }

    // 设置消费者组ID
    errCode = globalConfig->set("group.id", groupId, errorStr);
    if (errCode != RdKafka::Conf::CONF_OK) {
        spdlog::error("KafkaConsumer: Failed to set group.id: {}", errorStr);
        return;
    }

    // 设置消费者系统级事件回调
    errCode = globalConfig->set("event_cb", eventCb, errorStr);
    if (errCode != RdKafka::Conf::CONF_OK) {
        spdlog::error("KafkaConsumer: Failed to set event_cb: {}", errorStr);
        return;
    }

    // 设置消费者的重新平衡回调
    errCode = globalConfig->set("rebalance_cb", rebalanceCb, errorStr);
    if (errCode != RdKafka::Conf::CONF_OK) {
        spdlog::error("KafkaConsumer: Failed to set rebalance_cb: {}", errorStr);
        return;
    }

     // 设置分区分配策略（每个分区最多被一个消费者消费以避免重复消费，这里设置分配的策略）
     errCode = globalConfig->set("partition.assignment.strategy", "range", errorStr);
     if (errCode != RdKafka::Conf::CONF_OK) {
         spdlog::error("KafkaConsumer: Failed to set partition.assignment.strategy: {}", errorStr);
         return;
     }

     // 心跳探活超时时间，用于检测消费者是否还活着
     errCode = globalConfig->set("session.timeout.ms", "6000", errorStr);
     if (errCode != RdKafka::Conf::CONF_OK) {
         spdlog::error("KafkaConsumer: Failed to set session.timeout.ms: {}", errorStr);
         return;
     }
     
     // 心跳保活间隔，用于检测消费者是否还活着，值可用于调节rebanlance频率
     errCode = globalConfig->set("heartbeat.interval.ms", "2000", errorStr);
     if (errCode != RdKafka::Conf::CONF_OK) {
         spdlog::error("KafkaConsumer: Failed to set heartbeat.interval.ms: {}", errorStr);
         return;
     }

    // 设置消费者的起始消费偏移值
    topicConfig = RdKafka::Conf::create(RdKafka::Conf::CONF_TOPIC);
    errCode = topicConfig->set("auto.offset.reset", "earliest", errorStr);
    if (errCode != RdKafka::Conf::CONF_OK) {
        spdlog::error("KafkaConsumer: Failed to set auto.offset.reset: {}", errorStr);
        return;
    }

    // 设置默认Topic配置
    errCode = globalConfig->set("default_topic_conf", topicConfig, errorStr);
    if (errCode != RdKafka::Conf::CONF_OK) {
        spdlog::error("KafkaConsumer: Failed to set default_topic_conf: {}", errorStr);
        return;
    }

    // 创建消费者实例
    consumerInstance = RdKafka::KafkaConsumer::create(globalConfig,errorStr);
    if (!consumerInstance) {
        spdlog::error("KafkaConsumer: Failed to create consumer: {}", errorStr);
        return;
    }

}

void KafkaConsumer::consumeMsg(RdKafka::Message& msg, void *opaque) {
    std::string payloadStr = (msg.payload() != nullptr) ? std::string(static_cast<char*>(msg.payload()), msg.len()) : "";
    switch (msg.err()) {
        case RdKafka::ERR_NO_ERROR:
            spdlog::info("Consume message, Topic: {}, Partition: {}, Offset: {}, Key: {}, Payload: {}",
                         msg.topic_name(), msg.partition(), msg.offset(),
                         *msg.key(),
                         payloadStr);
            break;
        case RdKafka::ERR__TIMED_OUT:
            spdlog::info("Consume message timeout: {}", RdKafka::err2str(msg.err()));
            break;
        default:
            spdlog::error("Consume message error: {}", RdKafka::err2str(msg.err()));
            break;
    }


}

void KafkaConsumer::start() {
    // 订阅Topic
    RdKafka::ErrorCode errCode = consumerInstance->subscribe(topicNames);
    if (errCode != RdKafka::ERR_NO_ERROR) {
        spdlog::error("KafkaConsumer: Failed to subscribe topic: {}", RdKafka::err2str(errCode));
        return;
    }

    while(1){
        RdKafka::Message *msg = consumerInstance->consume(3000); 
        consumeMsg(*msg, nullptr);
        consumerInstance->commitAsync();    //异步提交，可能会重复消费，业务层需要处理
        delete msg;
    }

    consumerInstance->commitSync();
}

KafkaConsumer::~KafkaConsumer() {
    // 关闭并销毁消费者
    if (consumerInstance) {
        consumerInstance->close();
        delete consumerInstance;
    }
    // 销毁配置对象
    if(globalConfig){
        delete globalConfig;
    }
    if(topicConfig){
        delete topicConfig;
    }
    // 销毁回调对象
    if (eventCb) {
        delete eventCb;
        eventCb = nullptr;
    }
    if (rebalanceCb) {
        delete rebalanceCb;
        rebalanceCb = nullptr;
    }
}