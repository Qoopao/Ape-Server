#ifndef GROUP_OUTBOX_WORKER_H
#define GROUP_OUTBOX_WORKER_H

#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <future>
#include <vector>

class IMessageProducer;

// outbox op=4 worker：GroupService 独占领取 operation=4。
//   stage=0：staging 正文 → Mongo(committed=false) → stage=1 → 删 staging
//   stage=1：PublishMessage(committed=true) → published_seq → Kafka group_topic → state=2
// 超过 GROUP_PUBLISH_MAX_ATTEMPTS 时：正文已存在则发布/放弃，不存在则写 tombstone；
// tombstone 写失败继续重试，绝不留下不可见空洞。
class GroupOutboxWorker {
public:
  explicit GroupOutboxWorker(IMessageProducer &producer);
  ~GroupOutboxWorker();

  void Start();
  void Stop();

private:
  boost::asio::awaitable<void> Run(int pool);
  // 已完成的 op=4 行按保留期删除；staging 孤儿按小时保留期清理。
  boost::asio::awaitable<void> RunMaintenance();

  IMessageProducer &producer_;
  std::atomic<bool> stopping_{false};
  std::vector<std::future<void>> workers_;
  std::future<void> maintenance_;
};

#endif
