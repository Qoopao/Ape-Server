#include "messagequeue/kafkaconsumer.h"
#include "messagequeue/kafkahandler.h"
#include "util/ioc_pool.h"
#include <exception>
#include <librdkafka/rdkafkacpp.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <optional>
#include <chrono>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

void KafkaConsumer::setHandler(std::shared_ptr<MessageHandler> handler) {
  this->msgHandler = handler;
}

void KafkaConsumer::setIOCPool(IOC_Pool *pool) { this->ioc_pool_ = pool; }

void ConsumerEventCb::event_cb(RdKafka::Event &event) {
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
    spdlog::info("KafkaConsumer Throttle: {}, {}", event.broker_name(),
                 event.broker_id());
    break;
  default:
    spdlog::info("KafkaConsumer Event: {}", event.str());
    break;
  }
}

void ConsumerRebalanceCb::rebalance_cb(
    RdKafka::KafkaConsumer *consumer, RdKafka::ErrorCode err,
    std::vector<RdKafka::TopicPartition *> &partitions) {
  generation_.fetch_add(1);

  spdlog::info("KafkaConsumer Rebalance: {}", RdKafka::err2str(err));

  if (err == RdKafka::ERR__ASSIGN_PARTITIONS) {
    consumer->assign(partitions);
  } else if (err == RdKafka::ERR__REVOKE_PARTITIONS) {
    consumer->unassign();
  }
}

KafkaConsumer::KafkaConsumer(const std::string &groupId,
                             const std::vector<std::string> &topicNames,
                             const std::string &brokerList)
    : globalConfig(nullptr), topicConfig(nullptr), eventCb(nullptr), rebalanceCb(nullptr), consumerInstance(nullptr) {
  this->brokerList = brokerList;
  this->groupId = groupId;
  this->topicNames = topicNames;
  this->eventCb = new ConsumerEventCb();
  this->rebalanceCb = new ConsumerRebalanceCb(generation_);

  RdKafka::Conf::ConfResult errCode;
  std::string errorStr;

  // 创建全局配置对象
  globalConfig = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
  errCode = globalConfig->set("bootstrap.servers", brokerList, errorStr);
  if (errCode != RdKafka::Conf::CONF_OK) {
    spdlog::error("KafkaConsumer: Failed to set bootstrap.servers: {}",
                  errorStr);
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
  errCode =
      globalConfig->set("partition.assignment.strategy", "range", errorStr);
  if (errCode != RdKafka::Conf::CONF_OK) {
    spdlog::error(
        "KafkaConsumer: Failed to set partition.assignment.strategy: {}",
        errorStr);
    return;
  }

  // 心跳探活超时时间，用于检测消费者是否还活着
  errCode = globalConfig->set("session.timeout.ms", "6000", errorStr);
  if (errCode != RdKafka::Conf::CONF_OK) {
    spdlog::error("KafkaConsumer: Failed to set session.timeout.ms: {}",
                  errorStr);
    return;
  }

  // 心跳保活间隔，用于检测消费者是否还活着，值可用于调节rebanlance频率
  errCode = globalConfig->set("heartbeat.interval.ms", "2000", errorStr);
  if (errCode != RdKafka::Conf::CONF_OK) {
    spdlog::error("KafkaConsumer: Failed to set heartbeat.interval.ms: {}",
                  errorStr);
    return;
  }

  // 关闭自动提交
  errCode = globalConfig->set("enable.auto.commit", "false", errorStr);
  if (errCode != RdKafka::Conf::CONF_OK) {
    spdlog::error("KafkaConsumer: Failed to set enable.auto.commit: {}",
                  errorStr);
    return;
  }
  if (globalConfig->set("enable.auto.offset.store", "false", errorStr) != RdKafka::Conf::CONF_OK)
    throw std::runtime_error("Kafka offset configuration failed: " + errorStr);

  // 设置消费者的起始消费偏移值
  topicConfig = RdKafka::Conf::create(RdKafka::Conf::CONF_TOPIC);
  errCode = topicConfig->set("auto.offset.reset", "earliest", errorStr);
  if (errCode != RdKafka::Conf::CONF_OK) {
    spdlog::error("KafkaConsumer: Failed to set auto.offset.reset: {}",
                  errorStr);
    return;
  }

  // 设置默认Topic配置
  errCode = globalConfig->set("default_topic_conf", topicConfig, errorStr);
  if (errCode != RdKafka::Conf::CONF_OK) {
    spdlog::error("KafkaConsumer: Failed to set default_topic_conf: {}",
                  errorStr);
    return;
  }

  // 创建消费者实例
  consumerInstance = RdKafka::KafkaConsumer::create(globalConfig, errorStr);
  if (!consumerInstance) {
    spdlog::error("KafkaConsumer: Failed to create consumer: {}", errorStr);
    return;
  }
}

void KafkaConsumer::consumeMsg(RdKafka::Message &msg, void *opaque) {
  std::string payloadStr =
      (msg.payload() != nullptr)
          ? std::string(static_cast<char *>(msg.payload()), msg.len())
          : "";
  switch (msg.err()) {
  case RdKafka::ERR_NO_ERROR:
    spdlog::info("Consume message, Topic: {}, Partition: {}, Offset: {}, Key: "
                 "{}, Payload: {}",
                 msg.topic_name(), msg.partition(), msg.offset(), msg.key() ? *msg.key() : std::string(),
                 payloadStr);
    // 调用消息处理器处理业务逻辑（异步协程方式，使用独立的 IOC_Pool）
    if (msgHandler && ioc_pool_) {
      std::lock_guard lock(lanes_mutex_);
      auto& lane = lanes_[{msg.topic_name(), msg.partition()}];
      if (!lane) {
        lane = std::make_shared<PartitionLane>();
        lane->worker = boost::asio::co_spawn(ioc_pool_->get_ioc(msg.partition() % ioc_pool_->pool_size()),
            processPartition(lane), boost::asio::use_future);
      }
      std::lock_guard queueLock(lane->mutex);
      lane->jobs.push_back({msg.topic_name(), payloadStr, msg.partition(), msg.offset(), generation_.load()});
    }
    break;
  case RdKafka::ERR__TIMED_OUT:
    spdlog::debug("Consume message timeout: {}", RdKafka::err2str(msg.err()));
    break;
  default:
    spdlog::error("Consume message error: {}", RdKafka::err2str(msg.err()));
    break;
  }
}

void KafkaConsumer::start() {
  if (!consumerInstance) {
    spdlog::error("KafkaConsumer: consumerInstance is null, cannot start");
    return;
  }

  try {
    // 订阅Topic
    RdKafka::ErrorCode errCode = consumerInstance->subscribe(topicNames);
    if (errCode != RdKafka::ERR_NO_ERROR) {
      spdlog::error("KafkaConsumer: Failed to subscribe topic: {}",
                    RdKafka::err2str(errCode));
      return;
    }

    while (running_) {
      updateBackpressure();
      RdKafka::Message *msg = consumerInstance->consume(1000);
      consumeMsg(*msg, nullptr); // 内部处理完消息后再手动commit
      delete msg;
    }

    spdlog::info("KafkaConsumer: consume loop exited");

  } catch (const std::exception &e) {
    spdlog::error("KafkaConsumer::start exception: {}", e.what());
  } catch (...) {
    spdlog::error("KafkaConsumer::start unknown exception");
  }
}

void KafkaConsumer::shutdown() {
  spdlog::info("KafkaConsumer: shutting down...");
  running_ = false;
}

boost::asio::awaitable<void> KafkaConsumer::processPartition(std::shared_ptr<PartitionLane> lane)
{
  boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
  while (running_.load()) {
    std::optional<Job> job;
    {
      std::lock_guard lock(lane->mutex);
      if (!lane->jobs.empty()) {
        job = std::move(lane->jobs.front());
        lane->jobs.pop_front();
      }
    }
    if (!job) {
      timer.expires_after(std::chrono::milliseconds(100));
      co_await timer.async_wait(boost::asio::use_awaitable);
      continue;
    }
    while (running_.load() && job->generation == generation_.load()) {
      bool complete = false;
      try {
        co_await msgHandler->handle(job->topic, job->payload);
        if (job->generation != generation_.load() || !running_.load())
          break;
        std::unique_ptr<RdKafka::TopicPartition> position(
            RdKafka::TopicPartition::create(job->topic, job->partition, job->offset + 1));
        std::vector<RdKafka::TopicPartition*> positions{position.get()};
        const auto error = consumerInstance->commitAsync(positions);
        if (error != RdKafka::ERR_NO_ERROR)
          throw std::runtime_error("Kafka commit failed: " + RdKafka::err2str(error));
        complete = true;
      } catch (const std::exception& e) {
        spdlog::error("Kafka partition={} offset={} retrying: {}", job->partition, job->offset, e.what());
      }
      if (complete)
        break;
      timer.expires_after(std::chrono::seconds(1));
      co_await timer.async_wait(boost::asio::use_awaitable);
    }
  }
}

void KafkaConsumer::updateBackpressure()
{
  std::lock_guard lock(lanes_mutex_);
  for (auto& entry : lanes_) {
    auto& lane = entry.second;
    std::lock_guard queueLock(lane->mutex);
    if (lane->pauseGeneration != generation_.load()) {
      lane->pauseGeneration = generation_.load();
      lane->paused = false;
    }
    std::unique_ptr<RdKafka::TopicPartition> position(
        RdKafka::TopicPartition::create(entry.first.first, entry.first.second));
    std::vector<RdKafka::TopicPartition*> positions{position.get()};
    if (!lane->paused && lane->jobs.size() >= 128) {
      if (consumerInstance->pause(positions) == RdKafka::ERR_NO_ERROR)
        lane->paused = true;
    } else if (lane->paused && lane->jobs.size() <= 64) {
      if (consumerInstance->resume(positions) == RdKafka::ERR_NO_ERROR)
        lane->paused = false;
    }
  }
}

void KafkaConsumer::join() {
  if (consume_thread_.joinable()) {
    consume_thread_.join();
    spdlog::info("KafkaConsumer: consume thread joined");
  }
}

KafkaConsumer::~KafkaConsumer() {
  shutdown();
  join();
  for (auto& entry : lanes_) {
    try {
      entry.second->worker.get();
    } catch (const std::exception& e) {
      spdlog::error("Kafka partition worker shutdown: {}", e.what());
    }
  }
  // 关闭并销毁消费者
  if (consumerInstance) {
    consumerInstance->close();
    delete consumerInstance;
  }
  // 销毁配置对象
  if (globalConfig) {
    delete globalConfig;
  }
  if (topicConfig) {
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
