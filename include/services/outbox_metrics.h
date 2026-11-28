#ifndef APE_SERVICES_OUTBOX_METRICS_H
#define APE_SERVICES_OUTBOX_METRICS_H

namespace ape {
namespace obs {

// 启动进程级的 outbox 积压采集协程（30s 一次，写指标 gauge）。
// 幂等：同一进程内多次调用只启动一次，MsgService/GroupService 共用。
// 协程随进程退出，不做单独停止。
void StartOutboxMetricsPoller(int pool = 0);

} // namespace obs
} // namespace ape

#endif
