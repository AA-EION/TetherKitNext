#include "tetherkitnext/net/loopback_link.h"

#include "tetherkitnext/common/i18n.h"

#include <algorithm>
#include <format>
#include <iterator>

namespace tetherkitnext::net {

LoopbackLink::LoopbackLink(const LoopbackConfig& config) : config_(config) {
  inbound_.reserve(config_.inbound_capacity);
  sent_.reserve(config_.sent_capacity);
  read_storage_.reserve(config_.max_frames_per_batch);
  read_views_.reserve(config_.max_frames_per_batch);
}

void LoopbackLink::Interrupt() noexcept {
  interrupted_.store(true, std::memory_order_release);
}

bool LoopbackLink::PushInbound(std::span<const std::byte> frame) {
  if (frame.size() > config_.max_frame_bytes) {
    inbound_drops_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const std::lock_guard<std::mutex> guard(mutex_);
  if (inbound_.size() >= config_.inbound_capacity) {
    inbound_drops_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  inbound_.emplace_back(frame.begin(), frame.end());
  return true;
}

Result<ReadBatch> LoopbackLink::ReadFrames() {
  read_storage_.clear();
  read_views_.clear();

  if (interrupted_.load(std::memory_order_acquire)) {
    return ReadBatch{};
  }

  {
    const std::lock_guard<std::mutex> guard(mutex_);
    const std::size_t take = std::min(inbound_.size(), config_.max_frames_per_batch);
    read_storage_.insert(read_storage_.end(), std::make_move_iterator(inbound_.begin()),
                         std::make_move_iterator(inbound_.begin() +
                                                 static_cast<std::ptrdiff_t>(take)));
    inbound_.erase(inbound_.begin(), inbound_.begin() + static_cast<std::ptrdiff_t>(take));
  }

  // 视图必须在下一次 ReadFrames 之前有效，所以数据留在 read_storage_ 里。
  for (const std::vector<std::byte>& frame : read_storage_) {
    read_views_.push_back(
        FrameView{.data = frame.data(), .length = static_cast<std::uint32_t>(frame.size())});
  }
  return ReadBatch{.frames = read_views_, .kernel_drops = InboundDrops()};
}

Result<WriteResult> LoopbackLink::WriteFrames(FrameBatch frames) {
  const std::uint32_t call_index = write_calls_.fetch_add(1, std::memory_order_relaxed);
  const std::uint32_t fail_after = fail_writes_after_.load(std::memory_order_relaxed);
  if (fail_after != 0 && call_index >= fail_after) {
    return std::unexpected(Error::Generic(
        Tr(Msg::kNetLoopbackInjectedFailure, call_index + 1)));
  }

  WriteResult result;
  const std::lock_guard<std::mutex> guard(mutex_);
  for (const FrameView& frame : frames) {
    if (frame.length < kMinEthernetFrameBytes || frame.length > config_.max_frame_bytes) {
      ++result.frames_skipped;
      continue;
    }
    if (sent_.size() >= config_.sent_capacity) {
      // 与真实 BPF 一致：写不下就停，让调用方从 frames_written 看出没写完。
      break;
    }
    sent_.emplace_back(frame.data, frame.data + frame.length);
    ++result.frames_written;
    result.bytes_written += frame.length;
  }

  total_sent_frames_.fetch_add(result.frames_written, std::memory_order_relaxed);
  total_sent_bytes_.fetch_add(result.bytes_written, std::memory_order_relaxed);
  return result;
}

std::vector<std::vector<std::byte>> LoopbackLink::DrainSent() {
  const std::lock_guard<std::mutex> guard(mutex_);
  std::vector<std::vector<std::byte>> drained;
  drained.swap(sent_);
  sent_.reserve(config_.sent_capacity);
  return drained;
}

}  // namespace tetherkitnext::net
