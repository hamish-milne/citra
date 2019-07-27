// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <iostream>
#include <set>
#include <array>
#include "common/common_types.h"

namespace SaveState {

using SectionId = std::array<char, 5>;

class StateSource
{
public:
    virtual const SectionId Name() const = 0;
    virtual void Serialize(std::ostream &stream) const = 0;
    virtual void Deserialize(std::istream &stream) = 0;
};

class StateManager
{
public:
    void RegisterSource(StateSource &source);
    void Save(std::ostream &stream);
    void Load(std::istream &stream);

private:
    std::set<StateSource*> sources {};
};

}