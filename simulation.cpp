#include "simulation.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
namespace model {
void Simulation::scheduleAt(Tick time,std::string name,std::function<void()> action,EventPhase phase){
 if(time<now_)throw std::logic_error("cannot schedule event in the past");
 if(!action)throw std::invalid_argument("event action is empty");
 if(nextSequence_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("event sequence overflow");
 events_.push(Event{time,phase,nextSequence_++,std::move(name),std::move(action)});
}
void Simulation::scheduleAfter(Tick delay,std::string name,std::function<void()> action,EventPhase phase){
 if(delay>std::numeric_limits<Tick>::max()-now_)throw std::overflow_error("event time overflow");
 scheduleAt(now_+delay,std::move(name),std::move(action),phase);
}
void Simulation::requestStop(std::string reason){if(!stopRequested_){stopRequested_=true;stopReason_=std::move(reason);}}
void Simulation::run(std::size_t maxEvents){
 while(!events_.empty()&&!stopRequested_&&processedEvents_<maxEvents){
  Event event=events_.top();events_.pop();
  if(event.time<now_)throw std::logic_error("simulation time moved backwards");
  now_=event.time;++processedEvents_;
  if(traceEvents_)std::cout<<"["<<now_<<" ns] EVENT "<<event.name<<'\n';
  event.action();
 }
 if(!stopRequested_)stopReason_=events_.empty()?"EventQueueEmpty":"EventLimit";
}
}
