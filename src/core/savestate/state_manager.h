// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <iostream>
#include <memory>
#include <set>
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
    StateManager();
    ~StateManager();
    void RegisterSource(std::shared_ptr<StateSource> source);
    void Save(std::ostream &stream);
    void Load(std::istream &stream);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

}
