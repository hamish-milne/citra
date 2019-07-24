// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include "state_manager.h"

namespace Core {

void StateManager::RegisterSource(StateSource &source)
{
    sources.insert(source);
}

bool compare_sources(const StateSource &a, const StateSource &b)
{
    return a.Name() < b.Name();
}

void StateManager::Save(std::ostream &stream)
{
    std::sort(sources.begin(), sources.end(), &compare_sources);
    for (auto &source : sources)
    {
        stream.write(source.Name().data(), source.Name().size());
        auto lengthPos = stream.tellp();
        u32 length = 0;
        stream.write(reinterpret_cast<const char*>(&length), sizeof(u32));
        auto startPos = stream.tellp();
        source.Serialize(stream);
        auto endPos = stream.tellp();
        length = endPos - startPos;
        stream.seekp(lengthPos);
        stream.write(reinterpret_cast<const char*>(&length), sizeof(u32));
        stream.seekp(endPos);
    }
}

void StateManager::Load(std::istream &stream)
{
    while (!stream.eof())
    {
        std::array<char, 4> name {};
        stream.read(name.data(), name.size());
        u32 length {};
        stream.read(reinterpret_cast<char*>(&length), sizeof(length));
    }
}

}