#include "tetherkitnext/common/scheduling.h"

#include <pthread/qos.h>

#include "tetherkitnext/common/logging.h"

namespace tetherkitnext {

unsigned int QosClassFor(ThreadRole role) noexcept {
  switch (role) {
    case ThreadRole::kDataPath:
      return QOS_CLASS_USER_INTERACTIVE;
    case ThreadRole::kControl:
      return QOS_CLASS_USER_INITIATED;
    case ThreadRole::kAuxiliary:
      return QOS_CLASS_UTILITY;
  }
  return QOS_CLASS_DEFAULT;
}

void ConfigureCurrentThread(std::string_view name, ThreadRole role) noexcept {
  SetCurrentThreadName(name);

  const auto qos = static_cast<qos_class_t>(QosClassFor(role));
  // relative_priority 传 0 表示该 QoS 等级内的默认优先级。
  const int rc = ::pthread_set_qos_class_self_np(qos, 0);
  if (rc != 0) {
    // QoS 设置失败不影响功能，只影响性能，因此只告警不返回错误。
    TETHERKITNEXT_WARN_TR(Msg::kCommonThreadQosFailed, static_cast<int>(qos), rc);
  }
}

}  // namespace tetherkitnext
