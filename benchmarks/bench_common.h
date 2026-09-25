// 基础设施层微基准的注册入口。
#pragma once

namespace tetherkitnext::bench {

class Runner;

/// 把 tk_common 的全部微基准注册到 runner 上。
void RegisterCommonBenchmarks(Runner& runner);

}  // namespace tetherkitnext::bench
