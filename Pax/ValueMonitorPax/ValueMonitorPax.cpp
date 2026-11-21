/**
 * ValueMonitorPax — Value Monitor (v0.0.992): see what a Value wire carries
 *
 * The Value hub needed a way to watch a value (user, 2026-11-20). One generic
 * Value In; the node shows the current value large, the min / max seen, how
 * many values arrived, and a scrolling graph of the last seconds (drawn by the
 * node from the live read-only values, ~30 per second — a monitor, not a
 * recorder).
 *
 * Parameters: 0 Last (ro), 1 Min (ro), 2 Max (ro), 3 Count (ro), 4 Reset
 * (writing 1 clears min / max / count; reads back 0). Nothing goes out.
 */

#include "../PaxAPI.h"
#include <algorithm>

struct ValueMonitorPax
{
    float last = 0.f, mn = 0.f, mx = 0.f, count = 0.f;
    bool  any  = false;
};

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "Value Monitor", "Patchy Examples", "1.0.0", 4, PAX_API_VERSION };   // 4 = Value only
    return &d;
}

PAX_Instance* PAX_create()                 { return new ValueMonitorPax(); }
void          PAX_destroy (PAX_Instance* i) { delete static_cast<ValueMonitorPax*> (i); }
void          PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<ValueMonitorPax*> (i);
    if (ctx->valuesIn)
        for (int e = 0; e < ctx->valueInCount; ++e)
        {
            const float v = ctx->valuesIn[e].value;
            a->last = v;
            if (! a->any) { a->mn = a->mx = v; a->any = true; }
            a->mn = std::min (a->mn, v);
            a->mx = std::max (a->mx, v);
            a->count += 1.f;
        }
    if (ctx->valueOutCount) *ctx->valueOutCount = 0;
    if (ctx->midiOutCount)  *ctx->midiOutCount  = 0;
}

int PAX_getParameterCount (PAX_Instance*) { return 5; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name = "Last";  info->minValue = -1e6f; info->maxValue = 1e6f; info->defaultValue = 0.f; info->step = 0.f; break;
        case 1: info->name = "Min";   info->minValue = -1e6f; info->maxValue = 1e6f; info->defaultValue = 0.f; info->step = 0.f; break;
        case 2: info->name = "Max";   info->minValue = -1e6f; info->maxValue = 1e6f; info->defaultValue = 0.f; info->step = 0.f; break;
        case 3: info->name = "Count"; info->minValue = 0.f;   info->maxValue = 1e9f; info->defaultValue = 0.f; info->step = 1.f; break;
        case 4: info->name = "Reset"; info->minValue = 0.f;   info->maxValue = 1.f;  info->defaultValue = 0.f; info->step = 1.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<ValueMonitorPax*> (i);
    switch (index) { case 0: return a->last; case 1: return a->mn; case 2: return a->mx; case 3: return a->count; default: return 0.f; }
}

void PAX_setParameter (PAX_Instance* i, int index, float v)
{
    auto* a = static_cast<ValueMonitorPax*> (i);
    if (index == 4 && v >= 0.5f) { a->mn = a->mx = a->last; a->count = 0.f; a->any = false; }
    // 0–3 are read-only (not restored: a monitor starts fresh)
}

int PAX_isParameterReadOnly (PAX_Instance*, int index) { return index <= 3 ? 1 : 0; }

int PAX_getValueInputCount() { return 1; }   // generic Value In

} // extern "C"
