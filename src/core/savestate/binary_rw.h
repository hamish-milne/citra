// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <iostream>
#include "common/common_types.h"

namespace SaveState {

struct BinaryReader
{
    std::istream &stream;

    template<typename T>
    T Read()
    {
        T retval{};
        stream.read(reinterpret_cast<char*>(&retval), sizeof(T));
        return retval;
    }

    template<typename T>
    void Read(T &sequence)
    {
        stream.read(reinterpret_cast<char*>(sequence.data()), sequence.size() * sizeof(T::value_type));
    }

    template<typename T>
    void Read(T *sequence, u32 size)
    {
        stream.read(reinterpret_cast<char*>(sequence), size * sizeof(T));
    }
};

struct BinaryWriter
{
    std::ostream &stream;

    template<typename T>
    void Write(const T &sequence)
    {
        stream.write(reinterpret_cast<const char*>(sequence.data()), sequence.size() * sizeof(T::value_type));
    }

    template<typename T>
    void Write(const T *sequence, u32 size)
    {
        stream.write(reinterpret_cast<const char*>(sequence), sizeof(T) * size);
    }

    template<typename T>
    void WriteSingle(const T &value)
    {
        stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }
};

}