#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <queue>
#include <string>
#include <vector>
namespace model {
using Tick=std::uint64_t;
struct Event {Tick time;std::uint64_t sequence;std::string name;std::function<void()> action;};
class Simulation {
public:
 Tick now() const noexcept{return now_;}
 std::size_t processedEvents() const noexcept{return processedEvents_;}
 bool stopRequested() const noexcept{return stopRequested_;}
 const std::string& stopReason() const noexcept{return stopReason_;}
 void scheduleAt(Tick time,std::string name,std::function<void()> action);
 void scheduleAfter(Tick delay,std::string name,std::function<void()> action);
 void requestStop(std::string reason);
 void run();
private:
 struct Later {bool operator()(const Event&a,const Event&b)const noexcept{return a.time!=b.time?a.time>b.time:a.sequence>b.sequence;}};
 Tick now_=0;
 std::uint64_t nextSequence_=0;
 std::size_t processedEvents_=0;
 bool stopRequested_=false;
 std::string stopReason_="NotStarted";
 std::priority_queue<Event,std::vector<Event>,Later> events_;
};
}
