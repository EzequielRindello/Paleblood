// SPDX-License-Identifier: GPL-2.0-or-later
// Frame timing: state methods that advance their owner by one fixed frame step.
//
// Small methods drive an owner object through its "advance" method, which takes a frame-time
// descriptor. The originals hard-code 1/30 s; the 60 FPS community patches rewrite it to 1/60 s,
// which BB_TARGET_FPS=60 does here (frame_time.h). The owner is the Chalice Dungeon gate's
// state (it sits at +0x108 in SprjDungeonGateIns, whose step table names the modes its states
// select). Nine virtual methods of objects that hold a pointer to it (built next to
// SprjHolygrail), in three shapes:
//   until_idle (5 copies with the owner at +0x8, 1 at +0x10): when busy, advance; then report
//     whether it is still busy.
//   request_10: when idle, set request 10, advance, clear the request.
//   request_7 (2 copies): when idle and in states 2..5, set request 7, advance, clear the
//     request; otherwise report that it cannot.
// And five methods of SprjDungeonGateIns itself: two steps of its step machine
// (STEP_Init_forChannelLoad, STEP_ModeSwitch) and three handlers of events it registered for,
// which store whether their request was taken. None of the five returns anything: on every path
// the originals leave only the stack guard's value in rax (or end in a void virtual call).
#include <cstddef>
#include <cstdint>

#include "frame_time.h"
#include "runtime/original.h"

namespace {

using frame_timing::FrameTime;

struct StateOwner;
struct StateOwnerVtable {
    void *unknown_slots[26];
    void (*advance)(StateOwner *, FrameTime *);
};
static_assert(offsetof(StateOwnerVtable, advance) == 0xd0);

// The gate's modes, as STEP_ModeSwitch reads them from the owner's state.
enum GateMode : uint32_t {
    mode_channel_load = 0,
    mode_no_holygrail = 1,
    mode_local_ritual = 2,
    mode_server_ritual = 3,
    mode_local_dungeon = 4,
    mode_server_dungeon = 5,
    mode_display_error = 6,
};

struct StateOwner {
    const StateOwnerVtable *vtable;
    uint8_t unknown_0x008[0x254];
    uint32_t state;  // a GateMode
    uint32_t request;
    int32_t busy;
    uint8_t unknown_0x268[0x4];
    float value_26c;  // set to 10 with request 2; its meaning is not known
};
static_assert(offsetof(StateOwner, state) == 0x25c && offsetof(StateOwner, request) == 0x260 &&
              offsetof(StateOwner, busy) == 0x264 && offsetof(StateOwner, value_26c) == 0x26c);

// The methods' own objects: the owner at +0x8, or at +0x10 for one of them.
struct StateAt8 {
    uint8_t unknown_0x00[0x8];
    StateOwner *owner;
};
struct StateAt10 {
    uint8_t unknown_0x00[0x10];
    StateOwner *owner;
};
static_assert(offsetof(StateAt8, owner) == 0x8 && offsetof(StateAt10, owner) == 0x10);

// The events the gate's handlers answer: the event's id, and flags it must carry.
struct GateEvent {
    uint8_t unknown_0x00[0x18];
    uint64_t flags;
};
static_assert(offsetof(GateEvent, flags) == 0x18);
constexpr uint64_t gate_event_required_flags = 0x1400;
constexpr int32_t gate_event_0 = 0, gate_event_1000 = 1000, gate_event_3000 = 3000;

struct DungeonGate;
struct DungeonGateVtable {
    void *unknown_slots[5];
    void (*set_step)(DungeonGate *, int32_t step);  // an index into the step table
    void *unknown_slot_30;
    void (*method_38)(DungeonGate *);  // called after STEP_Init_forChannelLoad and a mode switch
};
static_assert(offsetof(DungeonGateVtable, set_step) == 0x28 && offsetof(DungeonGateVtable, method_38) == 0x38);

// SprjDungeonGateIns: a step machine (its base counts steps at +0x50) with the state at +0x108.
struct DungeonGate {
    const DungeonGateVtable *vtable;
    uint8_t unknown_0x008[0x100];
    StateOwner owner;
    bool request_taken;  // what the last event handler's request did
    uint8_t unknown_0x379[0x327];
    int32_t value_6a0;  // reset by the handler of event 3000
};
static_assert(offsetof(DungeonGate, owner) == 0x108 && offsetof(DungeonGate, request_taken) == 0x378 &&
              offsetof(DungeonGate, value_6a0) == 0x6a0);

// The step machine's step counter; past 27 steps it also builds a debug message.
RT_ORIGINAL(0x01ec3000, step_machine_count_step, void(DungeonGate *));

// What the methods return (the state machine's codes; their names are not known).
constexpr uint32_t result_still_busy = 1, result_done = 2, result_not_now = 4;
// The requests the owner is given while it advances.
constexpr uint32_t request_none = 0, request_1 = 1, request_2 = 2, request_10 = 10, request_7 = 7;
// What the handler of event 3000 leaves in value_6a0.
constexpr int32_t value_6a0_reset = -1;
// What request 2 sets value_26c to.
constexpr float request_2_value = 10.0f;

// Indices into SprjDungeonGateIns's step table (the steps that start each mode).
enum GateStep : int32_t {
    step_init_for_channel_load = 1,
    step_init_for_disp_error = 5,
    step_init_for_no_holygrail = 8,
    step_init_for_local_ritual = 11,
    step_init_for_server_ritual = 14,
    step_init_for_local_dungeon = 17,
    step_init_for_server_dungeon = 20,
};

// The step STEP_ModeSwitch goes to for each mode, in GateMode order.
constexpr GateStep mode_first_step[] = {
    step_init_for_channel_load,  step_init_for_no_holygrail,   step_init_for_local_ritual,
    step_init_for_server_ritual, step_init_for_local_dungeon,  step_init_for_server_dungeon,
    step_init_for_disp_error,
};
static_assert(sizeof mode_first_step / sizeof mode_first_step[0] == mode_display_error + 1);
// request_7 runs only in these states.
constexpr uint32_t request_7_first_state = 2, request_7_last_state = 5;

void advance(StateOwner *owner)
{
    FrameTime step = frame_timing::fixed_frame_time();
    owner->vtable->advance(owner, &step);
}

uint32_t until_idle(StateOwner *owner)
{
    if (owner->busy == 0) return result_done;
    advance(owner);
    return owner->busy != 0 ? result_still_busy : result_done;
}

void advance_with_request_10(StateOwner *owner)
{
    if (owner->busy != 0) return;
    owner->request = request_10;
    advance(owner);
    owner->request = request_none;
}

// The owner's requests that report whether they were taken (true) or not now (false).
bool take_request(StateOwner *owner, uint32_t request)
{
    owner->request = request;
    advance(owner);
    owner->request = request_none;
    return true;
}

bool take_request_1(StateOwner *owner)
{
    if (owner->state == mode_channel_load || owner->state == mode_display_error || owner->busy != 0) return false;
    return take_request(owner, request_1);
}

bool take_request_2(StateOwner *owner)
{
    if (owner->busy != 0 || owner->state != mode_channel_load) return false;
    owner->value_26c = request_2_value;
    return take_request(owner, request_2);
}

bool take_request_7(StateOwner *owner)
{
    if (owner->busy != 0 || owner->state < request_7_first_state || owner->state > request_7_last_state) return false;
    return take_request(owner, request_7);
}

bool take_request_10(StateOwner *owner)
{
    if (owner->busy != 0) return false;
    return take_request(owner, request_10);
}

bool is_gate_event(const GateEvent *event, int32_t id, int32_t wanted)
{
    return id == wanted && (event->flags & gate_event_required_flags) == gate_event_required_flags;
}

uint32_t advance_with_request_7(StateOwner *owner)
{
    if (owner->busy != 0 || owner->state < request_7_first_state || owner->state > request_7_last_state)
        return result_not_now;
    owner->request = request_7;
    advance(owner);
    owner->request = request_none;
    return result_done;
}

}  // namespace

extern "C" uint32_t bb_frame_timing_until_idle_01f6a9f0(StateAt8 *self) { return until_idle(self->owner); }
extern "C" uint32_t bb_frame_timing_until_idle_0200e100(StateAt8 *self) { return until_idle(self->owner); }
extern "C" uint32_t bb_frame_timing_until_idle_02012610(StateAt8 *self) { return until_idle(self->owner); }
extern "C" uint32_t bb_frame_timing_until_idle_020128f0(StateAt8 *self) { return until_idle(self->owner); }
extern "C" uint32_t bb_frame_timing_until_idle_02012bf0(StateAt8 *self) { return until_idle(self->owner); }
extern "C" uint32_t bb_frame_timing_until_idle_0200d8a0(StateAt10 *self) { return until_idle(self->owner); }

extern "C" void bb_frame_timing_request_10_02012780(StateAt8 *self) { advance_with_request_10(self->owner); }
extern "C" uint32_t bb_frame_timing_request_7_0200e270(StateAt8 *self) { return advance_with_request_7(self->owner); }
extern "C" uint32_t bb_frame_timing_request_7_02012a60(StateAt8 *self) { return advance_with_request_7(self->owner); }

// SprjDungeonGateIns::STEP_Init_forChannelLoad: once the gate is idle in ChannelLoad, request 2.
extern "C" void bb_frame_timing_dungeon_gate_step_init_for_channel_load(DungeonGate *gate)
{
    if (!take_request_2(&gate->owner)) return;
    step_machine_count_step(gate);
    gate->vtable->method_38(gate);
}

// SprjDungeonGateIns::STEP_ModeSwitch: finish what the state is doing, then go to the first step
// of its mode.
extern "C" void bb_frame_timing_dungeon_gate_step_mode_switch(DungeonGate *gate)
{
    StateOwner *owner = &gate->owner;
    if (owner->busy != 0) {
        advance(owner);
        if (owner->busy != 0) return;
    }
    if (owner->state > mode_display_error) return;
    gate->vtable->set_step(gate, mode_first_step[owner->state]);
    gate->vtable->method_38(gate);
}

// Event 0 asks for request 1, then for request 2 (which sees the state request 1 left). Request
// 1's answer is stored and then overwritten: as in the original, only request 2's answer is left,
// but the first store is visible to the owner while request 2 advances it.
extern "C" void bb_frame_timing_dungeon_gate_on_event_0(DungeonGate *gate, const GateEvent *event, int32_t id)
{
    if (!is_gate_event(event, id, gate_event_0)) return;
    gate->request_taken = take_request_1(&gate->owner);
    gate->request_taken = take_request_2(&gate->owner);
}

// Event 1000: request 7 (only when idle in states 2..5).
extern "C" void bb_frame_timing_dungeon_gate_on_event_1000(DungeonGate *gate, const GateEvent *event, int32_t id)
{
    if (!is_gate_event(event, id, gate_event_1000)) return;
    gate->request_taken = take_request_7(&gate->owner);
}

// Event 3000: request 10 (only when idle). value_6a0 is reset after the request whether or not it
// was taken, as in the original.
extern "C" void bb_frame_timing_dungeon_gate_on_event_3000(DungeonGate *gate, const GateEvent *event, int32_t id)
{
    if (!is_gate_event(event, id, gate_event_3000)) return;
    gate->request_taken = take_request_10(&gate->owner);
    gate->value_6a0 = value_6a0_reset;
}
