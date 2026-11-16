#include "simulation.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
namespace model {
void Simulation::scheduleAt(Tick time,std::string name,std::function<void()> action){
 if(time<now_)throw std::logic_error("cannot schedule event in the past");
 if(!action)throw std::invalid_argument("event action is empty");
 if(nextSequence_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("event sequence overflow");
 events_.push(Event{time,nextSequence_++,std::move(name),std::move(action)});
}
void Simulation::scheduleAfter(Tick delay,std::string name,std::function<void()> action){
 if(delay>std::numeric_limits<Tick>::max()-now_)throw std::overflow_error("event time overflow");
 scheduleAt(now_+delay,std::move(name),std::move(action));
}
void Simulation::requestStop(std::string reason){if(!stopRequested_){stopRequested_=true;stopReason_=std::move(reason);}}
void Simulation::run(){
 while(!events_.empty()&&!stopRequested_){
  Event event=events_.top();events_.pop();
  if(event.time<now_)throw std::logic_error("simulation time moved backwards");
  now_=event.time;++processedEvents_;
  std::cout<<"["<<now_<<" ns] EVENT "<<event.name<<'\n';
  event.action();
 }
 if(!stopRequested_)stopReason_="EventQueueEmpty";
}
}
