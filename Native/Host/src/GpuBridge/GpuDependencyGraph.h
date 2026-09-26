#pragma once
#include "GpuExecutionAbi.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
namespace native_runtime {
class GpuContractError:public std::runtime_error {
public:
    int32_t code;
    GpuContractError(int32_t value,const char* text):std::runtime_error(text),code(value){}
};
inline void gpuRequire(bool value,int32_t code,const char* text){if(!value)throw GpuContractError(code,text);}
// Serialized by the backend's submission lock. It owns no D3D device or second
// budget. The same graph arbitrates every producer/consumer queue and version.
class GpuDependencyGraph {
    struct Resource {
        uint64_t version=1;
        uint32_t state=0;
        std::array<uint64_t,3> reads{},writes{};
    };
    uint64_t generation_,revision_=0;
    std::array<uint64_t,3> submitted_{},completed_{};
    std::map<uint64_t,Resource> resources_;
public:
    struct Plan {
        uint64_t revision=0,generation=0;
        uint32_t queue=0;
        std::array<uint64_t,3> waits{};
        std::vector<NRC_GpuAccess> accesses;
    };
    explicit GpuDependencyGraph(uint64_t generation):generation_(generation){gpuRequire(generation!=0,NRC_GPU_INVALID,"Zero GPU device generation");}
    void add(uint64_t id,uint32_t state,uint64_t version=1){
        gpuRequire(id&&version,NRC_GPU_INVALID,"Invalid GPU resource identity");
        gpuRequire(revision_!=UINT64_MAX,NRC_GPU_INTERNAL,"GPU graph revision exhausted");
        gpuRequire(resources_.emplace(id,Resource{version,state,{},{}}).second,NRC_GPU_INVALID,"Duplicate GPU resource identity");++revision_;
    }
    bool contains(uint64_t id)const{return resources_.find(id)!=resources_.end();}
    uint64_t generation()const{return generation_;}
    uint64_t submitted(uint32_t queue)const{return submitted_.at(queue);}
    uint64_t completed(uint32_t queue)const{return completed_.at(queue);}
    void complete(uint32_t queue,uint64_t value){
        gpuRequire(queue<3&&value!=UINT64_MAX,NRC_GPU_REMOVED,"Invalid or removed GPU completion timeline");
        completed_[queue]=std::max(completed_[queue],value);
    }
    bool retired(uint64_t id)const{
        auto at=resources_.find(id);gpuRequire(at!=resources_.end(),NRC_GPU_STALE,"Unknown GPU resource");
        for(size_t q=0;q<3;++q)if(std::max(at->second.reads[q],at->second.writes[q])>completed_[q])return false;
        return true;
    }
    void remove(uint64_t id){
        gpuRequire(retired(id),NRC_GPU_PENDING,"GPU consumers still retain resource");
        gpuRequire(revision_!=UINT64_MAX,NRC_GPU_INTERNAL,"GPU graph revision exhausted");
        resources_.erase(id);++revision_;
    }
    Plan prepare(uint32_t queue,const NRC_GpuAccess* accesses,uint64_t count,const NRC_GpuFence* waits=nullptr,uint64_t waitCount=0)const{
        gpuRequire(queue<3&&(!count||accesses)&&(!waitCount||waits),NRC_GPU_INVALID,"Invalid GPU work packet");
        gpuRequire(count<=SIZE_MAX/sizeof(NRC_GpuAccess)&&waitCount<=SIZE_MAX/sizeof(NRC_GpuFence)&&revision_!=UINT64_MAX,NRC_GPU_INVALID,"GPU packet extent overflow");
        Plan plan;plan.generation=generation_;plan.revision=revision_;plan.queue=queue;
        if(count)plan.accesses.assign(accesses,accesses+static_cast<size_t>(count));
        std::set<uint64_t> seen;
        for(const auto& access:plan.accesses){
            auto found=resources_.find(access.resource);
            gpuRequire(found!=resources_.end(),NRC_GPU_STALE,"GPU packet references retired resource");
            const auto& prior=found->second;
            const bool write=(access.mode&NRC_GPU_WRITE)!=0;
            gpuRequire(!access.reserved&&access.mode&&!(access.mode&~3u)&&seen.insert(access.resource).second,NRC_GPU_INVALID,"Duplicate or invalid GPU resource access");
            gpuRequire(access.version==prior.version||(write&&prior.version!=UINT64_MAX&&access.version==prior.version+1),NRC_GPU_STALE,"GPU resource version differs");
            gpuRequire(access.before_state==prior.state,NRC_GPU_INVALID,"GPU resource boundary state differs");
            for(size_t q=0;q<3;++q){
                plan.waits[q]=std::max(plan.waits[q],prior.writes[q]);
                if(write||access.before_state!=access.after_state)plan.waits[q]=std::max(plan.waits[q],prior.reads[q]);
            }
        }
        for(uint64_t n=0;n<waitCount;++n){const auto& fence=waits[n];
            gpuRequire(!fence.reserved&&fence.generation==generation_&&fence.queue<3,NRC_GPU_STALE,"Foreign GPU fence timeline");
            gpuRequire(fence.value<=submitted_[fence.queue],NRC_GPU_INVALID,"Waiting for an unsubmitted future GPU fence is forbidden");
            plan.waits[fence.queue]=std::max(plan.waits[fence.queue],fence.value);
        }
        plan.waits[queue]=0;for(size_t q=0;q<3;++q)if(plan.waits[q]<=completed_[q])plan.waits[q]=0;
        return plan;
    }
    NRC_GpuFence commit(const Plan& plan,uint64_t value){
        gpuRequire(plan.generation==generation_&&plan.revision==revision_&&plan.queue<3,NRC_GPU_STALE,"Stale prepared GPU submission");
        // Graphics fences can also advance through their owning Unity queue.
        // A new submission must not reuse an already completed value: that
        // would make live buffers appear retired immediately after commit.
        gpuRequire(value>submitted_[plan.queue]&&value>completed_[plan.queue]&&value!=UINT64_MAX&&revision_!=UINT64_MAX,NRC_GPU_INVALID,"GPU submission fence did not advance past observed completion");
        // Resources and all metadata were allocated by prepare/add. Commit is
        // pointer/scalar assignment only under the backend's submission lock.
        for(const auto& access:plan.accesses){
            auto found=resources_.find(access.resource);
            gpuRequire(found!=resources_.end(),NRC_GPU_STALE,"GPU resource retired during prepared submission");
        }
        for(const auto& access:plan.accesses){auto& resource=resources_.find(access.resource)->second;
            resource.state=access.after_state;resource.version=access.version;
            if((access.mode&NRC_GPU_WRITE)||access.before_state!=access.after_state){
                resource.reads={};resource.writes={};resource.writes[plan.queue]=value;
            }else resource.reads[plan.queue]=value;
        }
        submitted_[plan.queue]=value;++revision_;
        return NRC_GpuFence{generation_,plan.queue,0,value};
    }
};
}
