// RNDIS 编解码微基准的注册入口。
#pragma once

namespace tetherkitnext::bench {

class Runner;

/// 把 RNDIS 编解码的微基准注册到 runner 上。
void RegisterRndisBenchmarks(Runner& runner);

}  // namespace tetherkitnext::bench
