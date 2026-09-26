#pragma once
#include "GpuExecutionAbi.h"
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
namespace native_runtime {
// Allocation accounting owned by the existing renderer residency scheduler.
// Bound allocations are already present in OS usage; only unbound reservations
// are subtracted again from OS headroom. No fixed partition or forced eviction.
class GpuAllocationLedger {
    struct Entry {NRC_GpuAllocation request{};bool bound=false;};
    mutable std::mutex mutex_;
    std::map<uint64_t,Entry> entries_;
    uint64_t next_=0,hardTarget_=0,rendererBytes_=0,denied_=0;
    std::array<uint64_t,2> budget_{},usage_{},reserved_{},bound_{};
    std::array<bool,2> known_{};
    static uint64_t sum(uint64_t a,uint64_t b){if(b>UINT64_MAX-a)throw std::overflow_error("GPU allocation accounting overflow");return a+b;}
public:
    struct Totals {
        uint64_t reservations=0,bytes=0,bound=0,denied=0,renderer=0;
        std::array<uint64_t,NRC_GPU_DOMAIN_COUNT> domains{};
        std::array<uint64_t,NRC_GPU_MEMORY_COUNT> memory{};
    };
    void budget(uint32_t segment,uint64_t bytes,uint64_t usage){
        if(segment>1)throw std::invalid_argument("Invalid GPU memory segment");
        std::lock_guard lock(mutex_);budget_[segment]=bytes;usage_[segment]=usage;known_[segment]=true;
    }
    void renderer(uint64_t bytes,uint64_t hardTarget){std::lock_guard lock(mutex_);rendererBytes_=bytes;hardTarget_=hardTarget;}
    uint64_t reserve(const NRC_GpuAllocation& request){
        if(request.size!=sizeof(request)||request.version!=1||!request.bytes||!request.owner||request.reserved||
            request.domain>=NRC_GPU_DOMAIN_COUNT||request.memory>=NRC_GPU_MEMORY_COUNT||request.nonlocal>1)
            throw std::invalid_argument("Invalid shared GPU allocation request");
        std::lock_guard lock(mutex_);const auto segment=request.nonlocal;
        const uint64_t unbound=reserved_[segment]-bound_[segment];
        const uint64_t headroom=budget_[segment]>usage_[segment]?budget_[segment]-usage_[segment]:0;
        const uint64_t held=sum(sum(reserved_[0],reserved_[1]),rendererBytes_);
        if(!known_[segment]||unbound>headroom||request.bytes>headroom-unbound||
            (hardTarget_&&(held>hardTarget_||request.bytes>hardTarget_-held))){if(denied_!=UINT64_MAX)++denied_;return 0;}
        if(next_==UINT64_MAX)throw std::overflow_error("GPU allocation identity exhausted");
        const auto bytes=sum(reserved_[segment],request.bytes);const auto id=next_+1;
        entries_.emplace(id,Entry{request,false});next_=id;reserved_[segment]=bytes;return id;
    }
    void bind(uint64_t id){
        std::lock_guard lock(mutex_);auto it=entries_.find(id);
        if(it==entries_.end()||it->second.bound)throw std::logic_error("Stale or duplicate GPU allocation binding");
        const auto& request=it->second.request;bound_[request.nonlocal]=sum(bound_[request.nonlocal],request.bytes);it->second.bound=true;
    }
    void release(uint64_t id){
        std::lock_guard lock(mutex_);auto it=entries_.find(id);
        if(it==entries_.end())throw std::logic_error("GPU allocation released twice");
        const auto& request=it->second.request;reserved_[request.nonlocal]-=request.bytes;
        if(it->second.bound)bound_[request.nonlocal]-=request.bytes;entries_.erase(it);
    }
    Totals totals()const{
        std::lock_guard lock(mutex_);Totals value;value.reservations=entries_.size();value.bytes=sum(reserved_[0],reserved_[1]);
        value.bound=sum(bound_[0],bound_[1]);value.denied=denied_;value.renderer=rendererBytes_;
        for(const auto& entry:entries_){const auto& request=entry.second.request;
            value.domains[request.domain]=sum(value.domains[request.domain],request.bytes);
            value.memory[request.memory]=sum(value.memory[request.memory],request.bytes);}
        return value;
    }
};
}
