// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "arm_interface.h"
#include "core/savestate/binary_rw.h"

const SaveState::SectionId ARM_Interface::Name() const { return {"CPU-"}; }

void ARM_Interface::Serialize(std::ostream &stream) const
{
    auto writer = SaveState::BinaryWriter{stream};
    for (auto i = 0; i < 15; i++) {
        writer.WriteSingle(GetReg(i));
    }
    writer.WriteSingle(GetPC());
    writer.WriteSingle(GetCPSR());
    for (auto i = 0; i < 64; i++) {
        writer.WriteSingle(GetVFPReg(i));
    }
    for (int i = 0; i < VFP_SYSTEM_REGISTER_COUNT; i++) {
        writer.WriteSingle(GetVFPSystemReg(static_cast<VFPSystemRegister>(i)));
    }
    for (int i = 0; i < CP15_REGISTER_COUNT; i++) {
        writer.WriteSingle(GetCP15Register(static_cast<CP15Register>(i)));
    }
}

void ARM_Interface::Deserialize(std::istream &stream)
{
    auto reader = SaveState::BinaryReader{stream};
    for (auto i = 0; i < 15; i++) {
        SetReg(i, reader.Read<u32>());
    }
    SetPC(reader.Read<u32>());
    SetCPSR(reader.Read<u32>());
    for (auto i = 0; i < 64; i++) {
        SetVFPReg(i, reader.Read<u32>());
    }
    for (int i = 0; i < VFP_SYSTEM_REGISTER_COUNT; i++) {
        SetVFPSystemReg(static_cast<VFPSystemRegister>(i), reader.Read<u32>());
    }
    for (int i = 0; i < CP15_REGISTER_COUNT; i++) {
        SetCP15Register(static_cast<CP15Register>(i), reader.Read<u32>());
    }
}