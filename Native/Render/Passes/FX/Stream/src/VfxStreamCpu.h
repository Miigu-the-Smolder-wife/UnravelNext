#pragma once
// CPU reference executor of the particle stream (include/NativeVfxStream.h).
// It implements the packet semantics exactly as the GPU module does, using the
// same formulas (shaders/VfxParticleMath.hlsli) in double. It reads only the
// packet (never the CPU authority), so it also validates the packet contract.
// It is O(particles) on the CPU: the executor of GPU-less contexts and tests,
// never the product path of a rendered World.
#include "NativeVfxStream.h"
// wfCurl in float (the World sampler's and the GPU's formula; WindField.hlsli in its C++ mode): UnravelNext's core
// math and its Atmosphere file when this copy is built there (Passes/FX/Stream/src -> Passes/Atmosphere), else
// Unravel's RuntimeCommon adapter.
#if __has_include("../../../Atmosphere/WindField.hlsli")
#include "unx/core/Math.h"
#include <cstring>
#include "../../../Atmosphere/WindField.hlsli"
#else
#include "../../RuntimeCommon/WindTurbulence.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace nv_stream {
template<class R> struct Vec2 {R x,y;};
template<class R> struct Vec3 {R x,y,z;};
template<class R> struct Vec4 {R x,y,z,w;};
template<class R> inline Vec3<R> operator+(Vec3<R> a,Vec3<R> b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
template<class R> inline Vec3<R> operator-(Vec3<R> a,Vec3<R> b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
template<class R> inline Vec3<R> operator-(Vec3<R> a){return {-a.x,-a.y,-a.z};}
template<class R> inline Vec3<R> operator*(Vec3<R> a,R s){return {a.x*s,a.y*s,a.z*s};}
typedef Vec2<double> Real2;typedef Vec3<double> Real3;typedef Vec4<double> Real4;
inline Real3 real3(const float* v){return {v[0],v[1],v[2]};}

// The shared formulas, instantiated with member buffers as hooks: in double for
// the reference executor (Math), and in float to measure how the GPU's float
// arithmetic conditions them (MathT<float>, tests only).
template<class R> struct MathT {
    typedef uint32_t uint;
    typedef R nv_real;
    typedef Vec2<R> nv_real2;
    typedef Vec3<R> nv_real3;
    typedef Vec4<R> nv_real4;
    static nv_real3 nv_make3(R x,R y,R z){return {x,y,z};}
    static nv_real4 nv_make4(R x,R y,R z,R w){return {x,y,z,w};}
    static R dot(nv_real3 a,nv_real3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
    static nv_real3 cross(nv_real3 a,nv_real3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
    static R length(nv_real3 a){return std::sqrt(dot(a,a));}
    static R sqrt(R v){return std::sqrt(v);}
    static R exp(R v){return std::exp(v);}
    static R pow(R a,R b){return std::pow(a,b);}
    static R cos(R v){return std::cos(v);}
    static R sin(R v){return std::sin(v);}
    static R abs(R v){return std::fabs(v);}
    static R min(R a,R b){return a<b?a:b;}
    static R max(R a,R b){return a>b?a:b;}
    static nv_real3 min(nv_real3 a,nv_real3 b){return {min(a.x,b.x),min(a.y,b.y),min(a.z,b.z)};}
    static nv_real3 max(nv_real3 a,nv_real3 b){return {max(a.x,b.x),max(a.y,b.y),max(a.z,b.z)};}
    static R clamp(R v,R a,R b){return v<a?a:v>b?b:v;}
    static R floor(R v){return std::floor(v);}
#define NV_PARTICLE_MATH_TYPES_ONLY
#include "../shaders/VfxParticleMath.hlsli"
#undef NV_PARTICLE_MATH_TYPES_ONLY
    const NvField* field_data=nullptr;uint field_count=0;
    const NvWorldField* world_data=nullptr;uint world_count=0;
    const NvSurface* surface_data=nullptr;uint surface_count=0;
    const NvHeightfield* height_data=nullptr;uint height_count=0;
    const NV_StreamHeightTile* tile_data=nullptr;
    const Real4* key_data=nullptr;
#define NV_FIELD_COUNT field_count
#define NV_FIELD(i) field_data[i]
#define NV_WORLD_FIELD_COUNT world_count
#define NV_WORLD_FIELD(i) world_data[i]
#define NV_SURFACE_COUNT surface_count
#define NV_SURFACE(i) surface_data[i]
#define NV_HEIGHTFIELD_COUNT height_count
#define NV_HEIGHTFIELD(i) height_data[i]
#define NV_HEIGHT(tile, sample) R(tile_data[tile].heights[sample])
#define NV_HEIGHT_HOLES(tile, word) tile_data[tile].holes[word]
#define NV_CURVE_KEY(i) key_data[i]
    // Wind turbulence (executor version 3): the World sampler's term in float - the record-local offset rounded to
    // float, times the CPU-prepared 1 / length, curl noise at the prepared phase, times rms.
    static nv_real3 wind_turbulence(nv_real3 d,const NvWorldField& f){
        const unx::float3 p{float(d.x),float(d.y),float(d.z)};
        const unx::float3 c=unx::render::wind::wfCurl(p*float(f.inv_length),float(f.phase),f.octaves_seed&255u,f.octaves_seed>>8)*float(f.rms);
        return {R(c.x),R(c.y),R(c.z)};
    }
#define NV_WIND_TURBULENCE(d, field) wind_turbulence(d, field)
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4100) // default candidate query ignores the segment
#endif
#include "../shaders/VfxParticleMath.hlsli"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#undef NV_FIELD_COUNT
#undef NV_FIELD
#undef NV_WORLD_FIELD_COUNT
#undef NV_WORLD_FIELD
#undef NV_SURFACE_COUNT
#undef NV_SURFACE
#undef NV_HEIGHTFIELD_COUNT
#undef NV_HEIGHTFIELD
#undef NV_HEIGHT
#undef NV_HEIGHT_HOLES
#undef NV_CURVE_KEY
#undef NV_WIND_TURBULENCE
};
typedef MathT<double> Math;

// A slot state in double (the in-process path keeps full precision).
struct ExactParticle {
    uint32_t row,birth;
    double position[3],velocity[3],age;
    double rotation[4]={0,0,0,1},spin[3]={0,0,0}; // NV_STREAM_PROGRAM_ORIENTATION slots (identity, 0 otherwise)
};

struct StreamFailure:std::runtime_error {using std::runtime_error::runtime_error;};
inline void stream_require(bool ok,const char* message){if(!ok)throw StreamFailure(message);}
template<class T> std::vector<T> stream_section(const uint8_t* data,uint64_t bytes,uint64_t offset,uint32_t count){
    std::vector<T> out(count);if(!count)return out;
    stream_require(offset%16==0&&offset>=sizeof(NV_StreamHeader)&&offset<=bytes&&uint64_t(count)*sizeof(T)<=bytes-offset,"stream section bounds");
    std::memcpy(out.data(),data+offset,size_t(count)*sizeof(T));return out;
}
// The per-tick fields of a row this tick (NV_StreamEmitterPatch).
inline void apply_emitter_patch(NV_StreamEmitter& e,const NV_StreamEmitterPatch& p){
    e.flags=p.flags;e.next_birth=p.next_birth;e.death_birth=p.death_birth;e.dying_birth=p.dying_birth;e.death_event=p.death_event;
    e.output_base=p.output_base;e.parent_event=p.parent_event;e.parent_row=p.parent_row;for(size_t a=0;a<3;++a)e.rebase[a]=p.rebase[a];
}
// The persistent emitter table after one packet (NV_STREAM_EMITTER_DELTA and patches).
// The CPU executor applies it, and the authority keeps the same table for what an
// executor holds (the patch decision), so both follow one rule.
inline void apply_emitter_table(std::vector<NV_StreamEmitter>& table,const NV_StreamHeader& h,const uint8_t* data,uint64_t bytes){
    if(h.flags&NV_STREAM_EMITTER_DELTA){
        // Persistent table: per-tick fields of unsent rows read as absent, then the listed rows.
        stream_require(!(h.flags&NV_STREAM_RESET)&&h.emitter_table>=table.size(),"emitter delta");
        const auto rows=stream_section<uint32_t>(data,bytes,h.emitter_rows,h.emitter_count);
        const auto blocks=stream_section<NV_StreamEmitter>(data,bytes,h.emitters,h.emitter_count);
        const auto patches=stream_section<NV_StreamEmitterPatch>(data,bytes,h.emitter_patches,h.emitter_patch_count);
        table.resize(h.emitter_table,NV_StreamEmitter{});
        for(auto& e:table){e.rebase[0]=e.rebase[1]=e.rebase[2]=0;e.flags&=~uint32_t(NV_STREAM_EMITTER_TRANSPORT|NV_STREAM_EMITTER_SOURCE|NV_STREAM_EMITTER_KILLED);e.parent_event=e.parent_row=NV_STREAM_NONE;}
        for(size_t n=0;n<rows.size();++n){stream_require(rows[n]<h.emitter_table&&(n==0||rows[n]>rows[n-1]),"emitter delta rows");table[rows[n]]=blocks[n];}
        for(size_t n=0;n<patches.size();++n){const auto& p=patches[n];
            stream_require(p.row<h.emitter_table&&(n==0||p.row>patches[n-1].row)&&!(p.flags&(NV_STREAM_EMITTER_TRANSPORT|NV_STREAM_EMITTER_SOURCE))&&
                           !std::binary_search(rows.begin(),rows.end(),p.row),"emitter patch rows");
            apply_emitter_patch(table[p.row],p);}
    }else{
        stream_require(h.emitter_count==h.emitter_table&&h.emitter_patch_count==0,"whole emitter table");
        table=stream_section<NV_StreamEmitter>(data,bytes,h.emitters,h.emitter_count);
    }
}

class CpuExecutor {
public:
    using Failure=StreamFailure;
    // exact: double restore states replacing the packet's float restore records
    // (same order and count), for in-process restore without float rounding.
    void submit(const uint8_t* data,uint64_t bytes,const std::vector<ExactParticle>* exact=nullptr){
        require(data&&bytes>=sizeof(NV_StreamHeader),"stream packet too small");
        NV_StreamHeader h;std::memcpy(&h,data,sizeof(h));
        require(h.magic==NV_STREAM_MAGIC&&h.version==NV_STREAM_VERSION&&h.bytes==bytes,"stream header");
        if(h.stream!=stream_||h.generation!=generation_){require(h.flags&NV_STREAM_RESET,"a new stream generation must start with RESET");}
        stream_=h.stream;generation_=h.generation;tick_=h.tick;
        if(h.flags&NV_STREAM_PROGRAMS){
            programs_=section<NV_StreamProgram>(data,bytes,h.programs,h.program_count);
            const auto keys=section<NV_StreamCurveKey>(data,bytes,h.curve_keys,h.curve_key_count);
            keys_.resize(keys.size());for(size_t k=0;k<keys.size();++k)keys_[k]={keys[k].t,keys[k].value[0],keys[k].value[1],keys[k].value[2]};
        }
        apply_emitter_table(emitters_,h,data,bytes);
        for(const auto& e:emitters_)require(!(e.flags&NV_STREAM_EMITTER_ACTIVE)||e.program<programs_.size(),"emitter program");
        const auto spawns=section<NV_StreamSpawn>(data,bytes,h.spawns,h.spawn_count);
        const auto explicits=section<NV_StreamExplicitBirth>(data,bytes,h.explicit_births,h.explicit_count);
        const auto fields=section<NV_StreamField>(data,bytes,h.fields,h.field_count);
        std::vector<NV_StreamWorldFieldTurbulent> world;
        if(h.flags&NV_STREAM_WIND_TURBULENCE)world=section<NV_StreamWorldFieldTurbulent>(data,bytes,h.world_fields,h.world_field_count);
        else for(const auto& w:section<NV_StreamWorldField>(data,bytes,h.world_fields,h.world_field_count)){NV_StreamWorldFieldTurbulent x{};x.field=w;world.push_back(x);}
        if(h.flags&(NV_STREAM_SURFACES|NV_STREAM_RESET)){
            surface_table_=section<NV_StreamSurface>(data,bytes,h.surfaces,h.surface_count);
            height_table_=section<NV_StreamHeightField>(data,bytes,h.height_fields,h.height_field_count);
            // Tiles of each heightfield consecutive, the triangle ranges consecutive (NV_StreamHeightField).
            uint64_t tiles=0,triangles=0;
            for(const auto& f:height_table_){
                require(f.cells_x>0&&f.cells_z>0&&f.tiles_x==(f.cells_x+15)/16&&f.tiles_z==(f.cells_z+15)/16&&f.first_tile==tiles&&f.first_triangle==triangles&&
                        f.spacing_x>0&&f.spacing_z>0,"heightfield table");
                tiles+=uint64_t(f.tiles_x)*f.tiles_z;triangles+=2ull*f.cells_x*f.cells_z;
            }
            require(tiles<=UINT32_MAX&&triangles<=UINT32_MAX,"heightfield table size");
            height_tiles_=section<NV_StreamHeightTile>(data,bytes,h.height_tiles,uint32_t(tiles));
        }
        const auto bodies=section<NV_StreamBody>(data,bytes,h.bodies,h.body_count);
        const auto dynamic=section<NV_StreamSurface>(data,bytes,h.dynamic_surfaces,h.dynamic_surface_count);
        const auto restore=section<NV_StreamParticle>(data,bytes,h.restore,h.restore_count);
        const auto orientations=(h.flags&NV_STREAM_ORIENTATION)?section<NV_StreamParticleOrientation>(data,bytes,h.restore_orientations,h.restore_count):std::vector<NV_StreamParticleOrientation>{};
        require(!(h.flags&NV_STREAM_ORIENTATION)||(h.flags&NV_STREAM_RESET),"orientation records come with RESET");
        // A state packet (dt == 0) keeps its tick's readback; only alive/status change.
        // It carries no body frames (its surface rows would not resolve) and integrates
        // nothing, so the tick inputs are loaded only for a simulating packet.
        const bool simulate=h.dt>0;
        if(simulate)load_inputs(fields,world,bodies,dynamic);
        if(simulate||(h.flags&NV_STREAM_RESET)){
            readback_={};readback_.stream=h.stream;readback_.generation=h.generation;readback_.tick=h.tick;
            events_.assign(simulate?h.event_slots:0u,NV_StreamEvent{});collisions_.clear();
        }
        if(h.flags&NV_STREAM_RESET){
            slots_.clear();
            require(!exact||exact->size()==restore.size(),"exact restore count");
            for(size_t n=0;n<restore.size();++n){
                Slot s{};const auto& r=restore[n];s.row=r.emitter;s.birth=r.birth;
                if(exact){const auto& x=(*exact)[n];require(x.row==r.emitter&&x.birth==r.birth,"exact restore identity");
                    s.state.position={x.position[0],x.position[1],x.position[2]};s.state.velocity={x.velocity[0],x.velocity[1],x.velocity[2]};s.state.age=x.age;
                    s.rotation={x.rotation[0],x.rotation[1],x.rotation[2],x.rotation[3]};s.spin={x.spin[0],x.spin[1],x.spin[2]};}
                else{s.state.position=real3(r.position);s.state.velocity=real3(r.velocity);s.state.age=r.age;
                    if(!orientations.empty()){const auto& o=orientations[n];s.rotation={o.rotation[0],o.rotation[1],o.rotation[2],o.rotation[3]};s.spin=real3(o.spin);}}
                require(s.row<emitters_.size(),"restore row");slots_.push_back(s);
            }
            require(slots_.size()<=h.slot_capacity,"restore capacity");
            // The GPU executors' rule (render A's resync gate): a row's restored births are the run starting at its
            // dying birth, [dying_birth, dying_birth + k) mod 2^32 - checked here too, so a packet the GPU refuses fails
            // the CPU reference first.
            std::vector<std::vector<uint32_t>> offsets(emitters_.size());
            for(const auto& s:slots_)offsets[s.row].push_back(s.birth-emitters_[s.row].dying_birth);
            for(auto& o:offsets){std::sort(o.begin(),o.end());for(size_t k=0;k<o.size();++k)require(o[k]==uint32_t(k),"restore births: not one of the row's contiguous births");}
        }
        origin_.resize(emitters_.size());inherited_.resize(emitters_.size());
        for(size_t e=0;e<emitters_.size();++e){origin_[e]=real3(emitters_[e].origin_anchor);inherited_[e]=real3(emitters_[e].inherited);}
        uint32_t status=0;
        if(simulate){
            // Depth 0: births, then every slot (existing and new).
            const size_t existing=slots_.size();
            for(uint32_t r=h.depth[0];r<h.depth[1];++r)spawn(h,spawns.at(r),status);
            for(const auto& x:explicits)spawn_explicit(h,x,status);
            for(size_t n=0;n<slots_.size();++n)advance(h,slots_[n],n<existing,status);
            // Depths 1..4: children of the previous depth's events.
            for(uint32_t d=1;d<=NV_STREAM_MAX_DEPTH;++d){
                if(h.depth[d]==h.depth[d+1])continue;
                for(uint32_t r=h.depth[d];r<h.depth[d+1];++r)resolve_child(spawns.at(r).emitter);
                const size_t first=slots_.size();
                for(uint32_t r=h.depth[d];r<h.depth[d+1];++r)spawn(h,spawns.at(r),status);
                for(size_t n=first;n<slots_.size();++n)advance(h,slots_[n],false,status);
            }
        }
        // Kills (also in state packets), then compaction.
        for(auto& s:slots_)if(s.alive&&(emitters_.at(s.row).flags&NV_STREAM_EMITTER_KILLED))s.alive=false;
        slots_.erase(std::remove_if(slots_.begin(),slots_.end(),[](const Slot& s){return !s.alive;}),slots_.end());
        for(auto& s:slots_)s.fresh=false;
        if(slots_.size()!=h.alive_after)status|=NV_STREAM_STATUS_ALIVE_MISMATCH;
        if(slots_.size()>h.slot_capacity)status|=NV_STREAM_STATUS_CAPACITY;
        readback_.alive=static_cast<uint32_t>(slots_.size());readback_.status|=status;
        if(simulate){readback_.collision_events=static_cast<uint32_t>(collisions_.size());events_.insert(events_.end(),collisions_.begin(),collisions_.end());}
        dt_=h.dt;
    }
    uint64_t stream()const{return stream_;}
    uint64_t generation()const{return generation_;}
    uint64_t tick()const{return tick_;}
    const NV_StreamCounters& counters()const{return readback_;}
    const std::vector<NV_StreamEvent>& events()const{return events_;}
    std::vector<ExactParticle> checkpoint()const{
        std::vector<ExactParticle> result;result.reserve(slots_.size());
        for(const auto& s:slots_)result.push_back({s.row,s.birth,{s.state.position.x,s.state.position.y,s.state.position.z},{s.state.velocity.x,s.state.velocity.y,s.state.velocity.z},s.state.age,
                                                   {s.rotation.x,s.rotation.y,s.rotation.z,s.rotation.w},{s.spin.x,s.spin.y,s.spin.z}});
        std::sort(result.begin(),result.end(),[](const ExactParticle& a,const ExactParticle& b){return a.row!=b.row?a.row<b.row:a.birth<b.birth;});
        return result;
    }
private:
    struct Slot {uint32_t row=0,birth=0;Math::NvState state{};bool alive=true,fresh=false;double pending=0;Real4 rotation{0,0,0,1};Real3 spin{0,0,0};};
    bool oriented(uint32_t row)const{return (programs_.at(emitters_.at(row).program).flags&NV_STREAM_PROGRAM_ORIENTATION)!=0;}
    static void require(bool ok,const char* message){stream_require(ok,message);}
    template<class T> static std::vector<T> section(const uint8_t* data,uint64_t bytes,uint64_t offset,uint32_t count){return stream_section<T>(data,bytes,offset,count);}
    static Real3 rotate(const float* q,Real3 v){
        const Real3 u{q[0],q[1],q[2]};const double w=q[3];
        const Real3 t=Math::cross(u,v)*2.0;return v+t*w+Math::cross(u,t);
    }
    void load_inputs(const std::vector<NV_StreamField>& fields,const std::vector<NV_StreamWorldFieldTurbulent>& world,const std::vector<NV_StreamBody>& bodies,const std::vector<NV_StreamSurface>& dynamic){
        std::vector<NV_StreamSurface> surfaces=surface_table_;surfaces.insert(surfaces.end(),dynamic.begin(),dynamic.end());
        fields_.clear();for(const auto& f:fields)fields_.push_back({real3(f.position),f.kind,real3(f.value),f.radius});
        world_.clear();for(const auto& t:world){const auto& w=t.field;Math::NvWorldField x{};x.origin=real3(w.origin);x.quantity=w.packed&255u;x.shape=(w.packed>>8)&255u;x.operation=(w.packed>>16)&255u;
            x.basis0=real3(w.inverse_basis);x.basis1=real3(w.inverse_basis+3);x.basis2=real3(w.inverse_basis+6);x.value=real3(w.value);
            x.rms=t.rms;x.inv_length=t.inv_length;x.phase=t.phase;x.octaves_seed=t.octaves_seed;world_.push_back(x);}
        surfaces_.clear();for(const auto& s:surfaces){Math::NvSurface x{};x.kind=s.kind;x.entity0=s.entity[0];x.entity1=s.entity[1];x.generation0=s.generation0;x.generation1=s.generation1;
            x.radius=s.radius;x.a=real3(s.a);x.b=real3(s.b);x.c=real3(s.c);x.velocity=real3(s.velocity);x.angular=real3(s.angular_velocity);x.origin=real3(s.origin);
            if(s.body!=NV_STREAM_NONE){require(s.body<bodies.size(),"surface body");const auto& b=bodies[s.body];
                // Offsets from the centre of mass: R p_local + (position - center).
                const Real3 lever=real3(b.position)-real3(b.center);
                x.a=rotate(b.rotation,x.a)+lever;x.b=rotate(b.rotation,x.b)+lever;x.c=rotate(b.rotation,x.c)+lever;x.velocity=real3(b.velocity);x.angular=real3(b.angular_velocity);x.origin=real3(b.center);}
            surfaces_.push_back(x);}
        // Heightfields: the body frame places origin and axes (the body is static: its surfaces do not move).
        heights_.clear();
        for(const auto& f:height_table_){require(f.body<bodies.size(),"heightfield body");const auto& b=bodies[f.body];
            require(b.velocity[0]==0&&b.velocity[1]==0&&b.velocity[2]==0&&b.angular_velocity[0]==0&&b.angular_velocity[1]==0&&b.angular_velocity[2]==0,"heightfield bodies are static");
            Math::NvHeightfield x{};x.entity0=f.entity[0];x.entity1=f.entity[1];x.generation0=f.generation0;x.generation1=f.generation1;
            x.cells_x=f.cells_x;x.cells_z=f.cells_z;x.first_tile=f.first_tile;x.tiles_x=f.tiles_x;x.first_triangle=f.first_triangle;
            x.origin=rotate(b.rotation,real3(f.origin))+real3(b.position);
            x.axis_x=rotate(b.rotation,Real3{double(f.spacing_x),0,0});x.axis_y=rotate(b.rotation,Real3{0,1,0});x.axis_z=rotate(b.rotation,Real3{0,0,double(f.spacing_z)});
            heights_.push_back(x);}
        math_.field_data=fields_.data();math_.field_count=uint32_t(fields_.size());
        math_.world_data=world_.data();math_.world_count=uint32_t(world_.size());
        math_.surface_data=surfaces_.data();math_.surface_count=uint32_t(surfaces_.size());
        math_.height_data=heights_.data();math_.height_count=uint32_t(heights_.size());math_.tile_data=height_tiles_.data();
        math_.key_data=keys_.data();
    }
    Math::NvMotion motion(uint32_t row,uint32_t birth){
        const auto& e=emitters_.at(row);const auto& p=programs_.at(e.program);Math::NvMotion m{};
        m.acceleration=real3(p.acceleration);m.drag=p.drag;m.noise=real3(p.noise);m.noise_frequency=p.noise_frequency;
        m.wind=(p.flags&NV_STREAM_PROGRAM_WIND)?1u:0u;m.collision=(p.flags&NV_STREAM_PROGRAM_COLLISION)?1u:0u;m.self=(p.flags&NV_STREAM_PROGRAM_COLLIDE_SELF)?1u:0u;
        m.entity0=e.entity[0];m.entity1=e.entity[1];m.generation0=e.generation[0];m.generation1=e.generation[1];
        m.restitution=p.restitution;m.friction=p.friction;m.separation=p.separation;m.origin_anchor=origin_[row];
        m.noise_seed=math_.nv_noise_seed(e.noise_key,birth);return m;
    }
    void write_event(uint32_t slot,uint32_t row,uint32_t birth,uint32_t kind,const Math::NvState& s,uint32_t& status){
        if(slot==NV_STREAM_NONE)return;
        if(slot>=events_.size()){status|=NV_STREAM_STATUS_RANGE;return;}
        NV_StreamEvent& ev=events_[slot];ev={};ev.emitter=row;ev.birth=birth;ev.kind=kind;
        ev.position[0]=float(s.position.x);ev.position[1]=float(s.position.y);ev.position[2]=float(s.position.z);
        ev.velocity[0]=float(s.velocity.x);ev.velocity[1]=float(s.velocity.y);ev.velocity[2]=float(s.velocity.z);
    }
    // Integrate a state for h seconds with its own drag factors (births, deaths).
    bool interval(uint32_t row,uint32_t birth,double h,Math::NvState& s,Math::NvImpact& impact){
        const auto m=motion(row,birth);return math_.nv_integrate(m,h,math_.nv_linear_drag(m.drag,h),s,impact);
    }
    static bool finite(const Math::NvState& s){
        return std::isfinite(s.position.x)&&std::isfinite(s.position.y)&&std::isfinite(s.position.z)&&
               std::isfinite(s.velocity.x)&&std::isfinite(s.velocity.y)&&std::isfinite(s.velocity.z)&&std::isfinite(s.age);
    }
    void collision_event(uint32_t row,uint32_t birth,const Math::NvImpact& impact,double h){
        const auto& p=programs_.at(emitters_.at(row).program);
        if(!impact.count||!(p.flags&NV_STREAM_PROGRAM_COLLISION_EVENTS))return;
        NV_StreamEvent ev{};ev.emitter=row;ev.birth=birth;ev.kind=NV_STREAM_EVENT_COLLISION;ev.impacts=impact.count;
        ev.position[0]=float(impact.contact.x);ev.position[1]=float(impact.contact.y);ev.position[2]=float(impact.contact.z);
        ev.velocity[0]=float(impact.velocity.x);ev.velocity[1]=float(impact.velocity.y);ev.velocity[2]=float(impact.velocity.z);
        ev.normal[0]=float(impact.normal.x);ev.normal[1]=float(impact.normal.y);ev.normal[2]=float(impact.normal.z);
        ev.after=float((1-impact.fraction)*h);collisions_.push_back(ev);
    }
    Math::NvState birth_state(const NV_StreamHeader& h,uint32_t row,uint32_t birth,double elapsed){
        const auto& e=emitters_.at(row);const auto& p=programs_.at(e.program);
        Math::NvBirthShape b{};b.shape=p.shape;b.position_radius=p.position_radius;b.velocity_radius=p.velocity_radius;b.box=real3(p.box);
        b.cone=real3(p.cone);b.cone_cos=p.cone_cos;b.velocity=real3(p.velocity);b.speed=e.speed;
        auto rows=[](const float* m,Real4& r0,Real4& r1,Real4& r2){r0={m[0],m[1],m[2],m[3]};r1={m[4],m[5],m[6],m[7]};r2={m[8],m[9],m[10],m[11]};};
        Real4 p0,p1,p2,c0,c1,c2;rows(e.source_previous,p0,p1,p2);rows(e.source_current,c0,c1,c2);
        const double fraction=h.dt>0?std::clamp((h.dt-elapsed)/h.dt,0.0,1.0):1.0;
        return math_.nv_birth_state(b,math_.nv_birth_rng(e.rng_key,birth),(e.flags&NV_STREAM_EMITTER_SOURCE)?1u:0u,p0,p1,p2,c0,c1,c2,real3(e.spawn_offset),inherited_[row],fraction);
    }
    void place(uint32_t row,uint32_t birth,Math::NvState s,double elapsed,uint32_t& status){
        (void)status;Slot slot{};slot.row=row;slot.birth=birth;slot.state=s;slot.fresh=true;slot.pending=elapsed;
        if(oriented(row)){const auto& e=emitters_.at(row);const auto& p=programs_.at(e.program);
            math_.nv_orientation_birth(math_.nv_birth_rng(e.rng_key,birth),p.spin_min,p.spin_max,slot.rotation,slot.spin);}
        slots_.push_back(slot);
    }
    void spawn(const NV_StreamHeader& h,const NV_StreamSpawn& r,uint32_t& status){
        require(r.emitter<emitters_.size()&&r.expired<=r.count,"spawn record");
        const auto& p=programs_.at(emitters_[r.emitter].program);
        for(uint32_t k=0;k<r.count;++k){
            const uint32_t birth=r.first_birth+k;
            const double elapsed=math_.nv_birth_elapsed(r.kind,k,r.interval,r.carry,r.rate);
            auto s=birth_state(h,r.emitter,birth,elapsed);
            if(r.birth_event!=NV_STREAM_NONE)write_event(r.birth_event+k,r.emitter,birth,NV_STREAM_EVENT_BIRTH,s,status);
            if(k<r.expired){
                if(r.death_event!=NV_STREAM_NONE){Math::NvImpact impact{};interval(r.emitter,birth,p.lifetime,s,impact);write_event(r.death_event+k,r.emitter,birth,NV_STREAM_EVENT_DEATH,s,status);}
                continue;
            }
            place(r.emitter,birth,s,elapsed,status);
        }
    }
    void spawn_explicit(const NV_StreamHeader& h,const NV_StreamExplicitBirth& x,uint32_t& status){
        (void)h;require(x.emitter<emitters_.size(),"explicit birth row");
        const auto& p=programs_.at(emitters_[x.emitter].program);
        Math::NvState s{};s.position=real3(x.position);s.velocity=real3(x.velocity);s.age=0;
        if(x.birth_event!=NV_STREAM_NONE)write_event(x.birth_event,x.emitter,x.birth,NV_STREAM_EVENT_BIRTH,s,status);
        if(double(x.elapsed)>=double(p.lifetime)){
            if(x.death_event!=NV_STREAM_NONE){Math::NvImpact impact{};interval(x.emitter,x.birth,p.lifetime,s,impact);write_event(x.death_event,x.emitter,x.birth,NV_STREAM_EVENT_DEATH,s,status);}
            return;
        }
        place(x.emitter,x.birth,s,x.elapsed,status);
    }
    void resolve_child(uint32_t row){
        auto& e=emitters_.at(row);if(e.parent_event==NV_STREAM_NONE)return;
        require(e.parent_event<events_.size()&&e.parent_row<emitters_.size(),"child parent event");
        const auto& ev=events_[e.parent_event];
        // Float sum by definition (NativeVfxStream.h, parent_event) on the parent's
        // origin of this tick: a parent created in this tick has no table value yet,
        // its origin was resolved at the previous depth (origin_ holds that float).
        const auto& parent=origin_[e.parent_row];
        origin_[row]={double(float(parent.x)+ev.position[0]),double(float(parent.y)+ev.position[1]),double(float(parent.z)+ev.position[2])};
        inherited_[row]=Real3{ev.velocity[0],ev.velocity[1],ev.velocity[2]}*double(e.inherited[0]);
        // The resolved values stay in the table for the ticks that do not resend the
        // row (NativeVfxStream.h, parent_event): the float sum and the float product.
        const float fraction=e.inherited[0];
        e.origin_anchor[0]=float(origin_[row].x);e.origin_anchor[1]=float(origin_[row].y);e.origin_anchor[2]=float(origin_[row].z);
        for(size_t a=0;a<3;++a)e.inherited[a]=fraction*ev.velocity[a];
    }
    void advance(const NV_StreamHeader& h,Slot& s,bool existing,uint32_t& status){
        if(!s.alive)return;
        const auto& e=emitters_.at(s.row);const auto& p=programs_.at(e.program);
        if(e.flags&NV_STREAM_EMITTER_KILLED){s.alive=false;return;}
        Math::NvImpact impact{};double h_used=h.dt;bool complete=true;
        if(existing){
            // Step a first: every existing slot is in this tick's origin space
            // (a death event reports its position in that space too).
            s.state.position=s.state.position-real3(e.rebase);
            if(e.flags&NV_STREAM_EMITTER_TRANSPORT){
                const float* t=e.transport;const Real4 r0{t[0],t[1],t[2],t[3]},r1{t[4],t[5],t[6],t[7]},r2{t[8],t[9],t[10],t[11]};
                s.state.position=math_.nv_affine_point(r0,r1,r2,s.state.position);s.state.velocity=math_.nv_affine_vector(r0,r1,r2,s.state.velocity);
                if(p.flags&NV_STREAM_PROGRAM_ORIENTATION){const Real4 q=math_.nv_quat_from_rows(r0,r1,r2);
                    s.rotation=math_.nv_quat_normalize(math_.nv_quat_mul(q,s.rotation));s.spin=math_.nv_quat_rotate(q,s.spin);}
            }
            const uint32_t span=e.death_birth-e.dying_birth,offset=s.birth-e.dying_birth;
            if(offset<span){
                if(e.flags&NV_STREAM_EMITTER_DEATH_EVENTS){
                    auto dead=s.state;interval(s.row,s.birth,std::max(0.0,double(p.lifetime)-dead.age),dead,impact);
                    write_event(e.death_event==NV_STREAM_NONE?NV_STREAM_NONE:e.death_event+offset,s.row,s.birth,NV_STREAM_EVENT_DEATH,dead,status);
                }
                s.alive=false;return;
            }
            Math::NvDrag d{e.drag_velocity,e.drag_position,e.drag_acceleration};
            complete=math_.nv_integrate(motion(s.row,s.birth),h.dt,d,s.state,impact);
        }else{
            h_used=s.pending;
            complete=interval(s.row,s.birth,s.pending,s.state,impact);
        }
        if(!complete)status|=NV_STREAM_STATUS_IMPACT_OVERFLOW;
        if(p.flags&NV_STREAM_PROGRAM_ORIENTATION){
            s.rotation=math_.nv_orientation_advance(s.rotation,s.spin,h_used);
            if(impact.count)s.spin=s.spin*(1.0-double(p.friction));
        }
        if(!finite(s.state)){status|=NV_STREAM_STATUS_NONFINITE;s.alive=false;return;}
        collision_event(s.row,s.birth,impact,h_used);
    }
    Math math_;
    uint64_t stream_=0,generation_=0,tick_=0;double dt_=0;
    std::vector<NV_StreamProgram> programs_;std::vector<Real4> keys_;
    std::vector<NV_StreamEmitter> emitters_;std::vector<Real3> origin_,inherited_;
    std::vector<Math::NvField> fields_;std::vector<Math::NvWorldField> world_;std::vector<Math::NvSurface> surfaces_;std::vector<NV_StreamSurface> surface_table_;
    std::vector<NV_StreamHeightField> height_table_;std::vector<NV_StreamHeightTile> height_tiles_;std::vector<Math::NvHeightfield> heights_;
    std::vector<Slot> slots_;
    std::vector<NV_StreamEvent> events_,collisions_;
    NV_StreamCounters readback_{};
};
}
