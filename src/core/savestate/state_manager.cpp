// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include "state_manager.h"

namespace Core {

void StateManager::RegisterSource(StateSource &source)
{
    sources.insert(&source);
}

void StateManager::Save(std::ostream &stream)
{
    for (auto &source : sources)
    {
        stream.write(source->Name().data(), source->Name().size()-1);
        auto lengthPos = stream.tellp();
        u32 length = 0;
        stream.write(reinterpret_cast<const char*>(&length), sizeof(u32));
        auto startPos = stream.tellp();
        source->Serialize(stream);
        auto endPos = stream.tellp();
        length = static_cast<u32>(endPos - startPos);
        stream.seekp(lengthPos);
        stream.write(reinterpret_cast<const char*>(&length), sizeof(u32));
        stream.seekp(endPos);
    }
}

void StateManager::Load(std::istream &stream)
{
    while (!stream.eof())
    {
        SectionId name {};
        stream.read(name.data(), name.size()-1);
        u32 length {};
        stream.read(reinterpret_cast<char*>(&length), sizeof(length));
        StateSource *source = nullptr;
        for (auto s : sources)
        {
            if (s->Name() == name) {
                source = s;
                break;
            }
        }
        if (source == nullptr) {
            // Log error - missing section
            stream.seekg(length, +1);
            continue;
        }
        source->Deserialize(stream);
    }
}

}