// Authored CPU spy. The generator inserts exact production method definitions below.
// It does not initialize Vulkan or establish native WSI/presentation completion.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#define EXIT_IF(c) do { if(c) throw std::runtime_error("strict source precondition"); } while(false)
#define EXIT(...) throw std::runtime_error("strict Vulkan fatal preserved")
#define LOGF(...) do {} while(false)
namespace Common { struct LockGuard { std::lock_guard<std::mutex> lock; explicit LockGuard(std::mutex& m):lock(m){} }; }
namespace vk {
using Semaphore=void*;using SwapchainKHR=void*;
enum class Result {eSuccess,eSuboptimalKHR,eErrorOutOfDateKHR,eErrorUnknown,eErrorSurfaceLostKHR,eErrorDeviceLost};
enum class StructureType {ePresentInfoKHR};enum class PipelineStageFlagBits {eTransfer};
inline std::string to_string(Result){return "owned mock result";}
struct PresentInfoKHR { StructureType sType{};uint32_t swapchainCount=0;const SwapchainKHR* pSwapchains=nullptr;
 const uint32_t* pImageIndices=nullptr;const Semaphore* pWaitSemaphores=nullptr;uint32_t waitSemaphoreCount=0; };
}
static vk::Semaphore Handle(uint32_t value){return reinterpret_cast<void*>(static_cast<uintptr_t>(value));}
struct SubmitInfo {
 vk::Semaphore wait=nullptr,signal=nullptr;uint64_t value=0;vk::PipelineStageFlagBits stage{};
 void AddWait(vk::Semaphore s,uint64_t v,vk::PipelineStageFlagBits p){wait=s;value=v;stage=p;}
 void AddSignal(vk::Semaphore s){signal=s;}
};
struct Binary { bool pending=false;uint64_t submitted_wait_tick=0; };
struct Model {
 std::mutex mutex;std::condition_variable cv;std::map<vk::Semaphore,Binary> binaries;
 std::vector<uint32_t> images{2,0,1,1,2,0,2,1,0};size_t image_cursor=0;
 uint64_t next_tick=7,complete_tick=0;uint32_t acquire_calls=0,violations=0,submissions=0;
 std::vector<uint64_t> waits;bool auto_complete=true,wait_entered=false,wait_failure=false,submit_failure=false;
 vk::Result acquire_result=vk::Result::eSuccess,present_result=vk::Result::eSuccess;
 vk::Semaphore last_submit_wait=nullptr,last_submit_signal=nullptr,last_present_wait=nullptr;
 void CompleteUnlocked(uint64_t tick){
  complete_tick=std::max(complete_tick,tick);
  for(auto& [handle,binary]:binaries)
   if(binary.submitted_wait_tick!=0 && binary.submitted_wait_tick<=complete_tick)binary={};
  cv.notify_all();
 }
 void Complete(uint64_t tick){std::lock_guard lock(mutex);CompleteUnlocked(tick);}
 bool AllAcquireSignalsConsumed(){for(const auto& [h,b]:binaries)if(b.pending)return false;return true;}
};
struct Device {
 Model& m;
 vk::Result acquireNextImageKHR(vk::SwapchainKHR,uint64_t timeout,vk::Semaphore s,void*,uint32_t* image){
  std::lock_guard lock(m.mutex);++m.acquire_calls;
  if(timeout!=std::numeric_limits<uint64_t>::max())throw std::runtime_error("timeout changed");
  if(m.binaries[s].pending){++m.violations;return vk::Result::eErrorUnknown;}
  const auto result=m.acquire_result;m.acquire_result=vk::Result::eSuccess;
  if(result==vk::Result::eSuccess || result==vk::Result::eSuboptimalKHR){
   *image=m.images[m.image_cursor++%m.images.size()];m.binaries[s]={true,0};
  }
  return result;
 }
};
struct Queue {
 Model& m;
 vk::Result presentKHR(vk::PresentInfoKHR* p){
  std::lock_guard lock(m.mutex);
  if(p->swapchainCount!=1 || p->waitSemaphoreCount!=1)throw std::runtime_error("present shape changed");
  m.last_present_wait=*p->pWaitSemaphores;auto result=m.present_result;m.present_result=vk::Result::eSuccess;return result;
 }
};
struct Graphics { Model& m;Device device;Queue queue;std::mutex queue_mutex;
 explicit Graphics(Model& model):m(model),device{model},queue{model}{} };
struct Window {Graphics graphic_ctx;explicit Window(Model& m):graphic_ctx(m){} };
struct CommandScheduler {
 Model& m;
 void Wait(uint64_t tick){
  std::unique_lock lock(m.mutex);m.waits.push_back(tick);
  if(tick==0 || tick<=m.complete_tick)return;
  m.wait_entered=true;m.cv.notify_all();
  if(m.wait_failure)throw std::runtime_error("owned device loss");
  if(m.auto_complete)m.CompleteUnlocked(tick);
  else m.cv.wait(lock,[&]{return m.complete_tick>=tick || m.wait_failure;});
  if(m.wait_failure)throw std::runtime_error("owned device loss");
 }
 uint64_t Submit(const SubmitInfo& s){
  std::lock_guard lock(m.mutex);
  if(m.submit_failure)throw std::runtime_error("owned submit failure");
  if(!m.binaries[s.wait].pending || s.value!=1 || s.stage!=vk::PipelineStageFlagBits::eTransfer)
   throw std::runtime_error("binary wait or transfer stage changed");
  const auto tick=m.next_tick++;m.binaries[s.wait].submitted_wait_tick=tick;
  ++m.submissions;m.last_submit_wait=s.wait;m.last_submit_signal=s.signal;return tick;
 }
};
class Swapchain {
public:
 enum class Status:uint8_t {Success,Recreate,SurfaceLost};
 Window& m_window;vk::SwapchainKHR m_handle=Handle(1);std::vector<int> m_images{0,1,2};
 std::vector<vk::Semaphore> m_image_acquired{Handle(100),Handle(101),Handle(102)};
 std::vector<vk::Semaphore> m_render_complete{Handle(300),Handle(301),Handle(302)};
 std::vector<uint64_t> m_image_acquired_ticks{0,0,0};
 uint32_t m_image_index=static_cast<uint32_t>(-1),m_frame_index=0;bool m_recreate_after_present=false;
 explicit Swapchain(Window& w):m_window(w){}
#ifdef BASELINE
 Status AcquireNextImage();
#else
 Status AcquireNextImage(CommandScheduler&);
#endif
 uint64_t Submit(CommandScheduler&);Status Present();
 Status Acquire(CommandScheduler& s){
#ifdef BASELINE
  (void)s;return AcquireNextImage();
#else
  return AcquireNextImage(s);
#endif
 }
};
#ifdef BASELINE
Swapchain::Status Swapchain::AcquireNextImage() {
	EXIT_IF(m_handle == nullptr || m_frame_index >= m_image_acquired.size());
	m_image_index     = static_cast<uint32_t>(-1);
	const auto result = m_window.graphic_ctx.device.acquireNextImageKHR(
	    m_handle, std::numeric_limits<uint64_t>::max(), m_image_acquired[m_frame_index], nullptr,
	    &m_image_index);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
	}
	EXIT_IF(m_image_index >= m_images.size());
	return Status::Success;
}

uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
	EXIT_IF(m_frame_index >= m_image_acquired.size() || m_image_index >= m_render_complete.size());
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	return scheduler.Submit(submit);
}

Swapchain::Status Swapchain::Present() {
	EXIT_IF(m_image_index >= m_render_complete.size());
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	vk::Result result;
	{
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		result = m_window.graphic_ctx.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	return Status::Success;
}
#else
Swapchain::Status Swapchain::AcquireNextImage(CommandScheduler& scheduler) {
	EXIT_IF(m_handle == nullptr || m_frame_index >= m_image_acquired.size() ||
	        m_frame_index >= m_image_acquired_ticks.size());
	// This is outside the renderer and queue mutexes. Waiting the slot's actual submit tick
	// proves its prior binary wait has completed; image availability alone does not.
	scheduler.Wait(m_image_acquired_ticks[m_frame_index]);
	m_recreate_after_present = false;
	m_image_index     = static_cast<uint32_t>(-1);
	const auto result = m_window.graphic_ctx.device.acquireNextImageKHR(
	    m_handle, std::numeric_limits<uint64_t>::max(), m_image_acquired[m_frame_index], nullptr,
	    &m_image_index);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			// Suboptimal is a successful acquisition and submits a binary signal. Consume it
			// through the normal render submission before queue-idle recreation.
			m_recreate_after_present = true;
			break;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
	}
	EXIT_IF(m_image_index >= m_images.size());
	return Status::Success;
}

uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
	EXIT_IF(m_frame_index >= m_image_acquired.size() ||
	        m_frame_index >= m_image_acquired_ticks.size() ||
	        m_image_index >= m_render_complete.size());
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	const auto tick = scheduler.Submit(submit);
	m_image_acquired_ticks[m_frame_index] = tick;
	return tick;
}

Swapchain::Status Swapchain::Present() {
	EXIT_IF(m_image_index >= m_render_complete.size());
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	vk::Result result;
	{
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		result = m_window.graphic_ctx.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	return m_recreate_after_present ? Status::Recreate : Status::Success;
}
#endif
static bool MatchesProductionSource(std::string text) {
 text.erase(std::remove(text.begin(),text.end(),'\r'),text.end());
 #ifdef BASELINE
const char* expected[] = {R"METHOD(Swapchain::Status Swapchain::AcquireNextImage() {
	EXIT_IF(m_handle == nullptr || m_frame_index >= m_image_acquired.size());
	m_image_index     = static_cast<uint32_t>(-1);
	const auto result = m_window.graphic_ctx.device.acquireNextImageKHR(
	    m_handle, std::numeric_limits<uint64_t>::max(), m_image_acquired[m_frame_index], nullptr,
	    &m_image_index);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
	}
	EXIT_IF(m_image_index >= m_images.size());
	return Status::Success;
})METHOD",R"METHOD(uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
	EXIT_IF(m_frame_index >= m_image_acquired.size() || m_image_index >= m_render_complete.size());
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	return scheduler.Submit(submit);
})METHOD",R"METHOD(Swapchain::Status Swapchain::Present() {
	EXIT_IF(m_image_index >= m_render_complete.size());
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	vk::Result result;
	{
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		result = m_window.graphic_ctx.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	return Status::Success;
})METHOD"};
#else
const char* expected[] = {R"METHOD(Swapchain::Status Swapchain::AcquireNextImage(CommandScheduler& scheduler) {
	EXIT_IF(m_handle == nullptr || m_frame_index >= m_image_acquired.size() ||
	        m_frame_index >= m_image_acquired_ticks.size());
	// This is outside the renderer and queue mutexes. Waiting the slot's actual submit tick
	// proves its prior binary wait has completed; image availability alone does not.
	scheduler.Wait(m_image_acquired_ticks[m_frame_index]);
	m_recreate_after_present = false;
	m_image_index     = static_cast<uint32_t>(-1);
	const auto result = m_window.graphic_ctx.device.acquireNextImageKHR(
	    m_handle, std::numeric_limits<uint64_t>::max(), m_image_acquired[m_frame_index], nullptr,
	    &m_image_index);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eSuboptimalKHR\n");
			// Suboptimal is a successful acquisition and submits a binary signal. Consume it
			// through the normal render submission before queue-idle recreation.
			m_recreate_after_present = true;
			break;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorUnknown:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorUnknown\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkAcquireNextImageKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkAcquireNextImageKHR failed: %s\n", vk::to_string(result).c_str());
	}
	EXIT_IF(m_image_index >= m_images.size());
	return Status::Success;
})METHOD",R"METHOD(uint64_t Swapchain::Submit(CommandScheduler& scheduler) {
	EXIT_IF(m_frame_index >= m_image_acquired.size() ||
	        m_frame_index >= m_image_acquired_ticks.size() ||
	        m_image_index >= m_render_complete.size());
	SubmitInfo submit;
	submit.AddWait(m_image_acquired[m_frame_index], 1, vk::PipelineStageFlagBits::eTransfer);
	submit.AddSignal(m_render_complete[m_image_index]);
	const auto tick = scheduler.Submit(submit);
	m_image_acquired_ticks[m_frame_index] = tick;
	return tick;
})METHOD",R"METHOD(Swapchain::Status Swapchain::Present() {
	EXIT_IF(m_image_index >= m_render_complete.size());
	const auto         ready = m_render_complete[m_image_index];
	vk::PresentInfoKHR present {};
	present.sType              = vk::StructureType::ePresentInfoKHR;
	present.swapchainCount     = 1;
	present.pSwapchains        = &m_handle;
	present.pImageIndices      = &m_image_index;
	present.pWaitSemaphores    = &ready;
	present.waitSemaphoreCount = 1;

	vk::Result result;
	{
		Common::LockGuard lock(m_window.graphic_ctx.queue_mutex);
		result = m_window.graphic_ctx.queue.presentKHR(&present);
	}
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eSuboptimalKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eSuboptimalKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorOutOfDateKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorOutOfDateKHR\n");
			return Status::Recreate;
		case vk::Result::eErrorSurfaceLostKHR:
			LOGF("vkQueuePresentKHR returned vk::Result::eErrorSurfaceLostKHR\n");
			return Status::SurfaceLost;
		default: EXIT("vkQueuePresentKHR failed: %s\n", vk::to_string(result).c_str());
	}
	m_frame_index = (m_frame_index + 1u) % static_cast<uint32_t>(m_images.size());
	return m_recreate_after_present ? Status::Recreate : Status::Success;
})METHOD"};
#endif
 for(const auto* body:expected)if(text.find(body)==std::string::npos)return false;
 return true;
}
static bool MatchesProductionFile(const char* path) {
 std::ifstream file(path,std::ios::binary);if(!file)return false;
 return MatchesProductionSource(std::string(std::istreambuf_iterator<char>(file),{}));
}
static void Require(bool pass,const char* what){if(!pass)throw std::runtime_error(what);}
struct Fixture {
 Model model;Window window{model};CommandScheduler scheduler{model};Swapchain swapchain{window};
 void Frame(){Require(swapchain.Acquire(scheduler)==Swapchain::Status::Success,"acquire failed");
  swapchain.Submit(scheduler);Require(swapchain.Present()==Swapchain::Status::Success,"present failed");}
 void ThreeFrames(){for(int i=0;i<3;++i)Frame();}
};
int main(int argc,char** argv){
 try {
  if(argc==3 && std::string(argv[1])=="--source-check-only")
   return MatchesProductionFile(argv[2])?0:3;
  if(argc!=1)return 2;
#ifndef SWAPCHAIN_SOURCE_PATH
  throw std::runtime_error("required production snapshot path not configured");
#else
  Require(MatchesProductionFile(SWAPCHAIN_SOURCE_PATH),"production methods changed: regenerate and review this exact-method fixture");
#endif
#ifdef BASELINE
  Fixture f;f.ThreeFrames();const auto status=f.swapchain.Acquire(f.scheduler);
  Require(status==Swapchain::Status::Recreate && f.model.violations==1,"baseline counterexample did not expose pending binary reuse");
  std::puts("EXPECTED RED actual original ring-wrap acquire reuses a pending binary semaphore");return 1;
#else
  unsigned groups=0;
  {
   Fixture f;f.ThreeFrames();Require(f.model.complete_tick==0 && f.model.submissions==3,"frames unexpectedly serialized");
   Require(f.model.waits==std::vector<uint64_t>({0,0,0}),"fresh slot waits are not zero");
   Require(f.swapchain.Acquire(f.scheduler)==Swapchain::Status::Success,"ring wrap did not retire");
   Require(f.model.waits.back()==7 && f.model.complete_tick==7 && f.model.violations==0,"wrong slot/global retirement");
   Require(f.model.binaries[Handle(101)].pending && f.model.binaries[Handle(102)].pending,"later frames were globally waited");
   Require(f.swapchain.Submit(f.scheduler)==10 && f.swapchain.m_image_acquired_ticks[0]==10,"actual returned tick not stored in reused slot");
   Require(f.model.last_submit_wait==Handle(100) && f.model.last_submit_signal==Handle(301),"frame acquire/image signal indexing conflated");
   f.swapchain.Present();Require(f.model.last_present_wait==Handle(301),"present binary lost image indexing");++groups;
  }
  {
   Fixture f;f.ThreeFrames();f.model.auto_complete=false;
   std::atomic<bool> returned=false;std::thread waiter([&]{Require(f.swapchain.Acquire(f.scheduler)==Swapchain::Status::Success,"delayed acquire failed");returned=true;});
   {std::unique_lock lock(f.model.mutex);f.model.cv.wait(lock,[&]{return f.model.wait_entered;});
    Require(f.model.acquire_calls==3 && !returned,"acquire ran before slot wait completed");}
   f.model.Complete(7);waiter.join();Require(returned && f.model.acquire_calls==4,"completion did not release acquire");++groups;
  }
  {
   Fixture f;f.ThreeFrames();f.model.Complete(9);f.model.auto_complete=false;
   Require(f.swapchain.Acquire(f.scheduler)==Swapchain::Status::Success && !f.model.wait_entered,"already-complete slot blocked");++groups;
  }
  {
   Fixture f;f.ThreeFrames();f.model.wait_failure=true;bool failed=false;
   try{f.swapchain.Acquire(f.scheduler);}catch(const std::runtime_error&){failed=true;}
   Require(failed && f.model.acquire_calls==3,"wait error was swallowed or acquisition continued");++groups;
  }
  {
   Fixture f;Require(f.swapchain.Acquire(f.scheduler)==Swapchain::Status::Success,"initial acquire failed");f.model.submit_failure=true;
   bool failed=false;try{f.swapchain.Submit(f.scheduler);}catch(const std::runtime_error&){failed=true;}
   Require(failed && f.swapchain.m_image_acquired_ticks[0]==0,"failed submit published a retirement tick");++groups;
  }
  {
   Fixture f;f.model.acquire_result=vk::Result::eSuboptimalKHR;
   Require(f.swapchain.Acquire(f.scheduler)==Swapchain::Status::Success && f.swapchain.m_recreate_after_present,"suboptimal acquisition discarded its signal");
   const auto tick=f.swapchain.Submit(f.scheduler);Require(f.swapchain.Present()==Swapchain::Status::Recreate,"suboptimal did not request recreation after submit/present");
   Require(f.model.binaries[Handle(100)].submitted_wait_tick==tick,"suboptimal binary signal lacks consuming submission");
   f.model.Complete(tick);Require(f.model.AllAcquireSignalsConsumed(),"queue completion did not consume suboptimal signal");++groups;
  }
  {
   for(auto result:{vk::Result::eErrorOutOfDateKHR,vk::Result::eErrorUnknown,vk::Result::eErrorSurfaceLostKHR}){
    Fixture f;f.model.acquire_result=result;const auto status=f.swapchain.Acquire(f.scheduler);
    Require(status==(result==vk::Result::eErrorSurfaceLostKHR?Swapchain::Status::SurfaceLost:Swapchain::Status::Recreate),"original acquire error route changed");
    Require(f.model.AllAcquireSignalsConsumed() && f.model.submissions==0,"failed acquire invented a consuming submission");
   }++groups;
  }
  {
   Fixture f;for(unsigned i=0;i<50;++i)f.Frame();Require(f.model.violations==0 && f.model.submissions==50,"multi-wrap lifecycle failed");++groups;
  }
  {
   Fixture f;f.Frame();f.swapchain.Acquire(f.scheduler);f.swapchain.Submit(f.scheduler);
   f.model.present_result=vk::Result::eErrorOutOfDateKHR;
   Require(f.swapchain.Present()==Swapchain::Status::Recreate && f.swapchain.m_image_acquired_ticks[1]==8,"present error lost actual acquisition retirement tick");++groups;
  }
  std::printf("PASS exact production Acquire/Submit/Present lifecycle spy groups=%u; CPU model only, no Vulkan runtime\n",groups);return 0;
#endif
 } catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 2;}
}
