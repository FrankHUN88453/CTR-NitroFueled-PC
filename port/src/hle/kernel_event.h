#pragma once
#include "../common.h"

struct Equeue;

constexpr s16 kFilterVideoOut = -13;
constexpr s16 kFilterGraphicsCore = -14;

void EqueueAdd(Equeue* eq, u64 ident, s16 filter, void* udata);
void EqueueDelete(Equeue* eq, u64 ident, s16 filter);
void EqueueTrigger(Equeue* eq, u64 ident, s16 filter, u64 data);
void EqueueTriggerFilter(Equeue* eq, s16 filter, u64 data);
