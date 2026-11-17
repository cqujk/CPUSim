#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <queue>
#include <string>
#include <vector>
namespace model {
using Tick=std::uint64_t;
enum class EventPhase : std::uint8_t { External = 0, CpuClock = 1 };
struct Event {Tick time;EventPhase phase;std::uint64_t sequence;std::string name;std::function<void()> action;};
class Simulation {
public:
 explicit Simulation(bool traceEvents=true):traceEvents_(traceEvents){}
 Tick now() const noexcept{return now_;}
 bool traceEvents() const noexcept{return traceEvents_;}
 std::size_t processedEvents() const noexcept{return processedEvents_;}
 bool stopRequested() const noexcept{return stopRequested_;}
 const std::string& stopReason() const noexcept{return stopReason_;}
 void scheduleAt(Tick time,std::string name,std::function<void()> action,
                 EventPhase phase=EventPhase::External);
 void scheduleAfter(Tick delay,std::string name,std::function<void()> action,
                    EventPhase phase=EventPhase::External);
 void requestStop(std::string reason);
 void run(std::size_t maxEvents=1000000);
private:
 struct Later {bool operator()(const Event&a,const Event&b)const noexcept{
  if(a.time!=b.time)return a.time>b.time;
  if(a.phase!=b.phase)return a.phase>b.phase;
  return a.sequence>b.sequence;
 }};
 Tick now_=0;
 std::uint64_t nextSequence_=0;
 std::size_t processedEvents_=0;
 bool stopRequested_=false;
 bool traceEvents_=true;
 std::string stopReason_="NotStarted";
 std::priority_queue<Event,std::vector<Event>,Later> events_;
};
}
