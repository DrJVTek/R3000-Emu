#pragma once

#include "gte.h"

namespace gte
{

// Experimental "modern" GTE backend scaffold.
//
// Important:
// - It is intentionally separate from the faithful PS1 GTE backend.
// - In this first refactor pass it still reuses the faithful implementation.
// - The goal is to make the primary GTE backend selectable at runtime before
//   introducing float / unsaturated projection experiments.
class GteModern final : public Gte
{
  public:
    GteModern() = default;
};

} // namespace gte
